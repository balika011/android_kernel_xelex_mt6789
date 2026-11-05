#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/input/matrix_keypad.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>

#define MAX_ROWS 10
#define MAX_COLS 8
#define MIN_ROWS 2
#define MIN_COLS 1
#define ROW_SHIFT 3
#define MATRIX_MAX_SIZE (MAX_ROWS * MAX_COLS)

/* in ms */
#define MAX_DEBOUNCE_TIME 20
#define MIN_DEBOUNCE_TIME 5
#define DEFAULT_DEBOUNCE 15

struct gpio_matrix_keypad {
	const struct matrix_keymap_data *keymap_data;
	struct input_dev *input;
	struct platform_device *pdev;

	u32 num_rows;
	u32 num_cols;
	u32 debounce_ms;
	bool wakeup;
	bool rep;

	unsigned short keycodes[MATRIX_MAX_SIZE];

	u16 keystate[MAX_ROWS];

	struct gpio_descs *gpiod_rows;
	struct gpio_descs *gpiod_cols;
};

static int gpio_matrix_keypad_scan_matrix(struct gpio_matrix_keypad *kp)
{
	u16 state[MAX_ROWS];
	int rc, row, col, code;
	DECLARE_BITMAP(row_values, MAX_ROWS);
	DECLARE_BITMAP(col_values, MAX_COLS);

	memset(state, 0, sizeof(state));

	// mdelay(kp->debounce_ms);

	rc = gpiod_get_array_value_cansleep(kp->num_cols, kp->gpiod_cols->desc,
					    kp->gpiod_cols->info, col_values);
	if (rc)
		return rc;

	/* Only scan the matrix if there's any key pressed. */
	if (!bitmap_full(col_values, kp->num_cols)) {
		bitmap_fill(row_values, MAX_ROWS);
		gpiod_set_array_value_cansleep(kp->num_rows,
					       kp->gpiod_rows->desc,
					       kp->gpiod_rows->info,
					       row_values);

		for (row = 0; row < kp->num_rows; row++) {
			gpiod_set_value_cansleep(kp->gpiod_rows->desc[row], 0);

			rc = gpiod_get_array_value_cansleep(
				kp->num_cols, kp->gpiod_cols->desc,
				kp->gpiod_cols->info, col_values);
			if (rc)
				return rc;

			for (col = 0; col < kp->num_cols; col++)
				if (!test_bit(col, col_values))
					state[row] |= BIT(col);

			gpiod_set_value_cansleep(kp->gpiod_rows->desc[row], 1);
		}

		bitmap_zero(row_values, MAX_ROWS);
		gpiod_set_array_value_cansleep(kp->num_rows,
					       kp->gpiod_rows->desc,
					       kp->gpiod_rows->info,
					       row_values);
	}

	for (row = 0; row < kp->num_rows; row++) {
		int bits_changed = state[row] ^ kp->keystate[row];

		if (!bits_changed)
			continue;

		for (col = 0; col < kp->num_cols; col++) {
			if (!(bits_changed & BIT(col)))
				continue;

			code = MATRIX_SCAN_CODE(row, col, ROW_SHIFT);
			input_event(kp->input, EV_MSC, MSC_SCAN, code);
			input_report_key(kp->input, kp->keycodes[code],
					 !!(state[row] & BIT(col)));
			input_sync(kp->input);
		}
	}

	memcpy(kp->keystate, state, sizeof(state));

	return 0;
}

static irqreturn_t gpio_matrix_keypad_irq(int irq, void *data)
{
	struct gpio_matrix_keypad *kp = data;
	int rc;

	rc = gpio_matrix_keypad_scan_matrix(kp);
	if (rc < 0)
		dev_err(&kp->pdev->dev, "failed to scan matrix\n");

	return IRQ_HANDLED;
}

static int gpio_matrix_keypad_parse_dt(struct gpio_matrix_keypad *kp)
{
	int rc;

	rc = matrix_keypad_parse_properties(&kp->pdev->dev, &kp->num_rows,
					    &kp->num_cols);
	if (rc < 0) {
		dev_err(&kp->pdev->dev,
			"Unable to parse matrix keypad properties\n");
		return rc;
	}

	if (kp->num_rows > MAX_ROWS) {
		dev_err(&kp->pdev->dev, "Too many rows\n");
		return -EINVAL;
	}

	if (kp->num_cols > MAX_COLS) {
		dev_err(&kp->pdev->dev, "Too many colums\n");
		return -EINVAL;
	}

	kp->gpiod_rows =
		devm_gpiod_get_array(&kp->pdev->dev, "row", GPIOD_OUT_LOW);
	if (IS_ERR(kp->gpiod_rows))
		return PTR_ERR(kp->gpiod_rows);

	if (kp->gpiod_rows->ndescs != kp->num_rows)
		return -EINVAL;

	kp->gpiod_cols =
		devm_gpiod_get_array(&kp->pdev->dev, "column", GPIOD_IN);
	if (IS_ERR(kp->gpiod_cols))
		return PTR_ERR(kp->gpiod_cols);

	if (kp->gpiod_cols->ndescs != kp->num_cols)
		return -EINVAL;

	rc = of_property_read_u32(kp->pdev->dev.of_node, "keypad,debounce-ms",
				  &kp->debounce_ms);
	if (rc < 0) {
		if (rc == -EINVAL)
			kp->debounce_ms = DEFAULT_DEBOUNCE;
		else {
			dev_err(&kp->pdev->dev,
				"Unable to parse 'debounce-ms'\n");
			return rc;
		}
	}

	kp->wakeup =
		of_property_read_bool(kp->pdev->dev.of_node, "keypad,wakeup");

	kp->rep = !of_property_read_bool(kp->pdev->dev.of_node,
					 "linux,keypad-no-autorepeat");

	return 0;
}

static int gpio_matrix_keypad_probe(struct platform_device *pdev)
{
	struct gpio_matrix_keypad *kp;
	int rc = 0, i;
	DECLARE_BITMAP(row_values, MAX_ROWS);

	kp = devm_kzalloc(&pdev->dev, sizeof(struct gpio_matrix_keypad),
			  GFP_KERNEL);
	if (!kp) {
		dev_err(&pdev->dev, "%s: Can't allocate gpio_matrix_keypad\n",
			__func__);
		return -ENOMEM;
	}

	kp->pdev = pdev;

	rc = gpio_matrix_keypad_parse_dt(kp);
	if (rc < 0) {
		dev_err(&pdev->dev, "Error parsing device tree\n");
		return rc;
	}

	/* the #rows and #columns are compulsary */
	if (!kp->num_cols || !kp->num_rows || kp->num_cols > MAX_COLS ||
	    kp->num_rows > MAX_ROWS || kp->num_cols < MIN_COLS ||
	    kp->num_rows < MIN_ROWS) {
		dev_err(&pdev->dev, "invalid rows/cols input data\n");
		return -EINVAL;
	}

	if (kp->debounce_ms > MAX_DEBOUNCE_TIME ||
	    kp->debounce_ms < MIN_DEBOUNCE_TIME || (kp->debounce_ms % 5 != 0)) {
		dev_err(&pdev->dev, "invalid debounce time supplied\n");
		return -EINVAL;
	}

	kp->input = input_allocate_device();
	if (!kp->input) {
		dev_err(&pdev->dev, "Can't allocate keypad input device\n");
		return -ENOMEM;
	}
	kp->input->dev.parent = &pdev->dev;

	kp->input->name = "qpnp_keypad";
	kp->input->phys = "qpnp_keypad/input0";
	kp->input->id.version = 0x0001;
	kp->input->id.product = 0x0001;
	kp->input->id.vendor = 0x0001;

	kp->input->evbit[0] = BIT_MASK(EV_KEY);

	if (kp->rep)
		set_bit(EV_REP, kp->input->evbit);

	kp->input->keycode = kp->keycodes;
	kp->input->keycodemax = MATRIX_MAX_SIZE;
	kp->input->keycodesize = sizeof(kp->keycodes);

	rc = matrix_keypad_build_keymap(kp->keymap_data, NULL, kp->num_rows,
					kp->num_cols, kp->keycodes, kp->input);
	if (rc < 0) {
		dev_err(&pdev->dev, "unable to build keymap\n");
		return rc;
	}

	input_set_capability(kp->input, EV_MSC, MSC_SCAN);
	input_set_drvdata(kp->input, kp);
	dev_set_drvdata(&pdev->dev, kp);

	/* initialize keypad state */
	memset(kp->keystate, 0, sizeof(kp->keystate));

	rc = input_register_device(kp->input);
	if (rc < 0) {
		dev_err(&pdev->dev, "unable to register keypad input device\n");
		return rc;
	}

	bitmap_zero(row_values, MAX_ROWS);
	gpiod_set_array_value_cansleep(kp->num_rows, kp->gpiod_rows->desc,
				       kp->gpiod_rows->info, row_values);

	for (i = 0; i < kp->num_rows; i++)
		gpiod_set_value_cansleep(kp->gpiod_rows->desc[i], 0);

	for (i = 0; i < kp->num_cols; i++) {
		rc = devm_request_threaded_irq(
			&pdev->dev, gpiod_to_irq(kp->gpiod_cols->desc[i]), NULL,
			gpio_matrix_keypad_irq,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"qpnp-keypad-sense", kp);
		if (rc) {
			dev_err(&pdev->dev, "failed to request col %d irq\n",
				i);
			return rc;
		}
	}

	device_init_wakeup(&pdev->dev, kp->wakeup);

	return rc;
}

static int gpio_matrix_keypad_remove(struct platform_device *pdev)
{
	struct gpio_matrix_keypad *kp = dev_get_drvdata(&pdev->dev);

	device_init_wakeup(&pdev->dev, 0);
	input_unregister_device(kp->input);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int gpio_matrix_keypad_suspend(struct device *dev)
{
	struct gpio_matrix_keypad *kp = dev_get_drvdata(dev);
	int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < kp->num_cols; i++) {
			enable_irq_wake(gpiod_to_irq(kp->gpiod_cols->desc[i]));
		}
	}

	return 0;
}

static int gpio_matrix_keypad_resume(struct device *dev)
{
	struct gpio_matrix_keypad *kp = dev_get_drvdata(dev);
	int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < kp->num_cols; i++) {
			disable_irq_wake(gpiod_to_irq(kp->gpiod_cols->desc[i]));
		}
	}

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(gpio_matrix_keypad_pm_ops, gpio_matrix_keypad_suspend, gpio_matrix_keypad_resume);

static struct of_device_id gpio_matrix_keypad_match_table[] = {
	{
		.compatible = "gpio-matrix-keypad",
	},
	{}
};

MODULE_DEVICE_TABLE(of, gpio_matrix_keypad_match_table);

static struct platform_driver gpio_matrix_keypad_driver = {
	.probe		= gpio_matrix_keypad_probe,
	.remove		= gpio_matrix_keypad_remove,
	.driver		= {
		.name = "gpio-matrix-keypad",
		.of_match_table = gpio_matrix_keypad_match_table,
		.owner = THIS_MODULE,
		.pm = &gpio_matrix_keypad_pm_ops,
	},
};

static int __init gpio_matrix_keypad_init(void)
{
	return platform_driver_register(&gpio_matrix_keypad_driver);
}
module_init(gpio_matrix_keypad_init);

static void __exit gpio_matrix_keypad_exit(void)
{
	platform_driver_unregister(&gpio_matrix_keypad_driver);
}
module_exit(gpio_matrix_keypad_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Balázs Triszka <info@balika011.hu>");
MODULE_DESCRIPTION("Driver for GPIO-based matrix keypads");
