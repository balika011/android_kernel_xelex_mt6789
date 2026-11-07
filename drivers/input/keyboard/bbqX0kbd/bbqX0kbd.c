// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keyboard Driver for Blackberry Keyboards BBQ10 from arturo182. Software written by wallComputer.
 * bbqX0kbd_main.c: Main C File.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/leds.h>

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
#include "mtk_panel_ext.h"
#include "mtk_disp_notify.h"
#endif

#include "config.h"
#include "bbqX0kbd_registers.h"
#include "debug_levels.h"
#include "bbq20kbd_pmod_codes.h"

enum bbqX0kbd_touch_mode {
	BBQX0KBD_TOUCH_MODE_TOUCHPAD,
	BBQX0KBD_TOUCH_MODE_DPAD
};

struct bbqX0kbd_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct class *class;
	struct input_dev *input_dev;

	struct regulator *vdd_reg;
	struct regulator *v18_reg;
	struct regulator *v28_reg;
	struct gpio_desc *dfu_gpio;
	struct led_classdev backlight;

	unsigned short keycode[NUM_KEYCODES];
	int8_t relX_offset;
	int8_t relY_offset;

	uint8_t version_number;

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	struct notifier_block disp_notifier;
#endif

	bool suspended;

	enum bbqX0kbd_touch_mode touch_mode;
};

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
static int bbqX0kbd_suspend(struct device *dev);
static int bbqX0kbd_resume(struct device *dev);
#endif

static irqreturn_t bbqX0kbd_irq_handler(int irq, void *dev_id)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_id;
	struct i2c_client *client = bbqX0kbd_data->client;
	struct input_dev *input_dev = bbqX0kbd_data->input_dev;
	int rc;
	unsigned int irq_reg;

	rc = regmap_read(bbqX0kbd_data->regmap, REG_INT, &irq_reg);
	if (rc < 0) {
		dev_err(&client->dev, "%s: Could not read REG_INT. Error: %d\n", __func__, rc);
		return IRQ_NONE;
	}

	if (irq_reg == 0)
		return IRQ_NONE;

	if (irq_reg & REG_INT_OVERFLOW)
		dev_warn(&client->dev, "%s overflow occurred.\n", __func__);

	if (irq_reg & REG_INT_KEY) {
		unsigned int count, i;

		rc = regmap_read(bbqX0kbd_data->regmap, REG_INT, &count);
		if (rc != 0) {
			dev_err(&client->dev, "%s Could not read REG_KEY, Error: %d\n",
				__func__, rc);
			return IRQ_NONE;
		}
		count = count & REG_KEY_KEYCOUNT_MASK;

		for (i = 0; i < count; i++) {
			uint8_t fifo_data[2], code, state;
			rc = regmap_bulk_read(bbqX0kbd_data->regmap, REG_FIF, fifo_data, sizeof(fifo_data));
			if (rc != 0) {
				dev_err(&client->dev,
					"%s Could not read REG_FIF, Error: %d\n",
					__func__, rc);
				return IRQ_NONE;
			}
			code = fifo_data[1];
			state = fifo_data[0];

			if (state == KEY_PRESSED_STATE || state == KEY_RELEASED_STATE) {
				uint16_t keycode = bbqX0kbd_data->keycode[code];

				input_event(input_dev, EV_MSC, MSC_SCAN, code);
				input_report_key(input_dev, keycode, state == KEY_PRESSED_STATE);
			}
		}
	}

	if (irq_reg & REG_INT_TOUCH) {
		unsigned int reg_x, reg_y;
		int8_t rel_x, rel_y;

		rc = regmap_read(bbqX0kbd_data->regmap, REG_TOX, &reg_x);
		if (rc < 0) {
			dev_err(&client->dev, "%s : Could not read REG_TOX. Error: %d\n", __func__, rc);
			return IRQ_NONE;
		}
		rel_x = (int8_t)reg_x;

		rc = regmap_read(bbqX0kbd_data->regmap, REG_TOY, &reg_y);
		if (rc < 0) {
			dev_err(&client->dev, "%s : Could not read REG_TOY. Error: %d\n", __func__, rc);
			return IRQ_NONE;
		}
		rel_y = (int8_t)reg_y;

		switch (bbqX0kbd_data->touch_mode) {
		case BBQX0KBD_TOUCH_MODE_TOUCHPAD:
			input_report_rel(input_dev, REL_X, rel_x);
			input_report_rel(input_dev, REL_Y, rel_y);
			break;

		case BBQX0KBD_TOUCH_MODE_DPAD: {
			uint16_t keycode;
			bbqX0kbd_data->relX_offset += rel_x;
			bbqX0kbd_data->relY_offset += rel_y;

			if (abs(bbqX0kbd_data->relY_offset) > BBQ20KBD_TRACKPAD_KEYS_OFFSET) {
				if (bbqX0kbd_data->relY_offset > 0) {
					keycode = KEY_DOWN;
				} else {
					keycode = KEY_UP;
				}
			}
			
			if(abs(bbqX0kbd_data->relX_offset) > BBQ20KBD_TRACKPAD_KEYS_OFFSET) {
				if (bbqX0kbd_data->relX_offset > 0) {
					keycode = KEY_RIGHT;
				} else {
					keycode = KEY_LEFT;
				}
			}

			if (keycode > 0) {
				input_report_key(input_dev, keycode, 1);
				input_report_key(input_dev, keycode, 0);
				bbqX0kbd_data->relY_offset = 0;
				bbqX0kbd_data->relX_offset = 0;
			}
			break;
		}
		}
	} else {
		bbqX0kbd_data->relX_offset = 0;
		bbqX0kbd_data->relY_offset = 0;
	}

	input_sync(input_dev);
	regmap_write(bbqX0kbd_data->regmap, REG_INT, 0);

	return IRQ_HANDLED;
}

static int bbqX0kbd_keyboard_probe_dt(struct device *dev,
				      struct bbqX0kbd_data *bbqX0kbd_data)
{
	bbqX0kbd_data->vdd_reg = devm_regulator_get(dev, "vdd");
	if (IS_ERR(bbqX0kbd_data->vdd_reg)) {
		dev_err(dev, "Failed to get vdd regulator\n");
		return PTR_ERR(bbqX0kbd_data->vdd_reg);
	}

	bbqX0kbd_data->v18_reg = devm_regulator_get(dev, "1v8");
	if (IS_ERR(bbqX0kbd_data->vdd_reg)) {
		dev_err(dev, "Failed to get vdd regulator\n");
		return PTR_ERR(bbqX0kbd_data->vdd_reg);
	}

	bbqX0kbd_data->v28_reg = devm_regulator_get(dev, "2v8");
	if (IS_ERR(bbqX0kbd_data->vdd_reg)) {
		dev_err(dev, "Failed to get vdd regulator\n");
		return PTR_ERR(bbqX0kbd_data->vdd_reg);
	}

	bbqX0kbd_data->dfu_gpio =
		devm_gpiod_get(dev, "firmware-download", GPIOD_OUT_LOW);
	if (IS_ERR(bbqX0kbd_data->dfu_gpio)) {
		dev_err(dev, "Failed to configure firmware download GPIO\n");
		return PTR_ERR(bbqX0kbd_data->dfu_gpio);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
int  bbqX0kbd_disp_notifier_callback(
			struct notifier_block *self,
			unsigned long event, void *data)
{
	struct bbqX0kbd_data *bbqX0kbd_data =
		container_of(self, struct bbqX0kbd_data, disp_notifier);
	int *evdata = (int *)data;
	if (event == MTK_DISP_EVENT_BLANK) {
		if (*evdata == MTK_DISP_BLANK_UNBLANK) {
			if (bbqX0kbd_data->suspended) {
				bbqX0kbd_resume(&(bbqX0kbd_data->client->dev));
			}
		}
	} else if (event == MTK_DISP_EARLY_EVENT_BLANK) {
		if (*evdata == MTK_DISP_BLANK_POWERDOWN) {
			if (!bbqX0kbd_data->suspended) {
				bbqX0kbd_suspend(&(bbqX0kbd_data->client->dev));
			}
		}
	}
	return 0;
}
#endif

static const struct regmap_config bbqX0kbd_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.use_single_read = true,
	.use_single_write = true,

	.disable_locking = true
};

static void bbqX0kbd_set_bl_level(struct led_classdev *led_cdev,
				  enum led_brightness value)
{
	struct bbqX0kbd_data *bbqX0kbd_data =
		container_of(led_cdev, struct bbqX0kbd_data, backlight);

	regmap_write(bbqX0kbd_data->regmap, REG_BKL, value);
}

static ssize_t get_firmware_upgrade(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n",
			  gpiod_get_value_cansleep(bbqX0kbd_data->dfu_gpio));
}

static ssize_t set_firmware_upgrade(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);

	if (buf[0] == '0') {
		regulator_disable(bbqX0kbd_data->vdd_reg);

		gpiod_set_value_cansleep(bbqX0kbd_data->dfu_gpio, 0);
		mdelay(100);

		regulator_enable(bbqX0kbd_data->vdd_reg);
	} else if (buf[0] == '1') {
		regulator_disable(bbqX0kbd_data->vdd_reg);

	} else if (buf[0] == '2') {
		gpiod_set_value_cansleep(bbqX0kbd_data->dfu_gpio, 1);

		regulator_enable(bbqX0kbd_data->vdd_reg);
	}

	return count;
}

static DEVICE_ATTR(firmware_upgrade, 0644, get_firmware_upgrade, set_firmware_upgrade);

static ssize_t get_switch_key_mouse(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", bbqX0kbd_data->touch_mode == BBQX0KBD_TOUCH_MODE_TOUCHPAD);
}

static ssize_t set_switch_key_mouse(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);

	if (buf[0] == '0') {
		bbqX0kbd_data->touch_mode = BBQX0KBD_TOUCH_MODE_DPAD;
		bbqX0kbd_data->keycode[0x05] = BTN_LEFT;
	} else if (buf[0] == '1') {
		bbqX0kbd_data->touch_mode = BBQX0KBD_TOUCH_MODE_TOUCHPAD;
		bbqX0kbd_data->keycode[0x05] = KEY_ENTER;
	}

	return count;
}

static DEVICE_ATTR(switch_key_mouse, 0644, get_switch_key_mouse, set_switch_key_mouse);

static int bbqX0kbd_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device *class_device;
	struct bbqX0kbd_data *bbqX0kbd_data;
	struct input_dev *input;
	int rc;
	int i;
	unsigned int registerValue;

#if (DEBUG_LEVEL & DEBUG_LEVEL_FE)
	dev_info(&client->dev, "%s Probing BBQX0KBD.\n", __func__);
#endif
	bbqX0kbd_data = devm_kzalloc(dev, sizeof(*bbqX0kbd_data), GFP_KERNEL);
	if (!bbqX0kbd_data)
		return -ENOMEM;

	bbqX0kbd_data->client = client;
	i2c_set_clientdata(client, bbqX0kbd_data);
	memcpy(bbqX0kbd_data->keycode, keycodes, sizeof(bbqX0kbd_data->keycode));

	bbqX0kbd_keyboard_probe_dt(&client->dev, bbqX0kbd_data);

	gpiod_set_value_cansleep(bbqX0kbd_data->dfu_gpio, 0);

	regulator_enable(bbqX0kbd_data->vdd_reg);

	mdelay(10);

	regulator_enable(bbqX0kbd_data->v18_reg);

	regulator_enable(bbqX0kbd_data->v28_reg);

	msleep(400);

	bbqX0kbd_data->regmap =
		devm_regmap_init_i2c(client, &bbqX0kbd_i2c_regmap);
	if (IS_ERR(bbqX0kbd_data->regmap)) {
		return PTR_ERR(bbqX0kbd_data->regmap);
	}

	rc = regmap_write(bbqX0kbd_data->regmap, REG_RST, 0);
	if (rc) {
		dev_err(&client->dev, "%s Could not Reset BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
	msleep(400);

	rc = regmap_read(bbqX0kbd_data->regmap, REG_VER, &registerValue);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not Read Version BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
	dev_info(&client->dev, "%s BBQX0KBD Software version: 0x%02X\n", __func__, registerValue);
	bbqX0kbd_data->version_number = registerValue;

	rc = regmap_write(bbqX0kbd_data->regmap, REG_CFG, REG_CFG_DEFAULT_SETTING);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not write configuration to BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
	rc =
		regmap_read(bbqX0kbd_data->regmap, REG_CFG, &registerValue);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not read REG_CFG. Error: %d\n", __func__, rc);
		return rc;
	}
	dev_info(&client->dev, "%s Configuration Register Value: 0x%02X\n", __func__, registerValue);
#endif

	rc = regmap_write(bbqX0kbd_data->regmap, REG_CF2, REG_CFG2_DEFAULT_SETTING);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not write configuration 2 to BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
	rc =
		regmap_read(bbqX0kbd_data->regmap, REG_CF2, &registerValue);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not read REG_CF2. Error: %d\n", __func__, rc);
		return rc;
	}
	dev_info(&client->dev, "%s Configuration 2 Register Value: 0x%02X\n", __func__, registerValue);
#endif

#if (BBQ20KBD_TRACKPAD_USE == BBQ20KBD_TRACKPAD_AS_MOUSE)
	bbqX0kbd_data->touch_mode = BBQX0KBD_TOUCH_MODE_TOUCHPAD;
	bbqX0kbd_data->keycode[0x05] = BTN_LEFT;
#elif (BBQ20KBD_TRACKPAD_USE == BBQ20KBD_TRACKPAD_AS_KEYS)
	bbqX0kbd_data->touch_mode = BBQX0KBD_TOUCH_MODE_DPAD;
	bbqX0kbd_data->keycode[0x05] = KEY_ENTER;
#endif

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(&client->dev, "%s Could not devm_input_allocate_device BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENOMEM;
	}
	bbqX0kbd_data->input_dev = input;

	input->name = "bbqX0kbd";
	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0001;
	input->keycode = bbqX0kbd_data->keycode;
	input->keycodesize = sizeof(bbqX0kbd_data->keycode[0]);
	input->keycodemax = ARRAY_SIZE(bbqX0kbd_data->keycode);

	for (i = 0; i < NUM_KEYCODES; i++)
		__set_bit(bbqX0kbd_data->keycode[i], input->keybit);

	__clear_bit(KEY_RESERVED, input->keybit);

	__set_bit(EV_REP, input->evbit);
	__set_bit(EV_KEY, input->evbit);

	input_set_capability(input, EV_MSC, MSC_SCAN);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);

	rc = regmap_write(bbqX0kbd_data->regmap, REG_BKL, 255);

	bbqX0kbd_data->backlight.name = "keyboard-backlight";
	bbqX0kbd_data->backlight.default_trigger = "kbd-backlight";
	bbqX0kbd_data->backlight.brightness_set = bbqX0kbd_set_bl_level;

	rc = devm_led_classdev_register(&client->dev,
					&bbqX0kbd_data->backlight);
	if (rc)
		dev_warn(
			&client->dev,
			"Unable to register keyboard backlight class dev (%d)\n",
			rc);

	rc = devm_request_threaded_irq(dev, client->irq, NULL, bbqX0kbd_irq_handler,
										IRQF_ONESHOT, client->name, bbqX0kbd_data);

	if (rc != 0) {
		dev_err(&client->dev, "Coudl not claim IRQ %d; error %d\n", client->irq, rc);
		return rc;
	}

	rc = input_register_device(input);
	if (rc != 0) {
		dev_err(dev, "Failed to register input device, error: %d\n", rc);
		return rc;
	}
	
#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	bbqX0kbd_data->disp_notifier.notifier_call =
		bbqX0kbd_disp_notifier_callback;
	if (mtk_disp_notifier_register("bbqX0kbd",
				       &bbqX0kbd_data->disp_notifier)) {
		dev_err(dev, "%s Failed to register disp notifier client.", __func__);
		return -ENOMEM;
	}
#endif

	bbqX0kbd_data->class = class_create(THIS_MODULE, "bbqX0kbd");
	if (IS_ERR(bbqX0kbd_data->class)) {
		rc = PTR_ERR(bbqX0kbd_data->class);
		bbqX0kbd_data->class = NULL;
		return rc;
	}

	class_device =
		device_create(bbqX0kbd_data->class, NULL, 0, NULL, "bbqX0kbd");
	if (IS_ERR(class_device)) {
		rc = PTR_ERR(class_device);
		return rc;
	}
	dev_set_drvdata(class_device, bbqX0kbd_data);
	device_create_file(class_device, &dev_attr_firmware_upgrade);
	device_create_file(class_device, &dev_attr_switch_key_mouse);

	device_init_wakeup(&client->dev, false);

	return 0;
}

static int bbqX0kbd_remove(struct i2c_client *client)
{
	struct bbqX0kbd_data *bbqX0kbd_data = i2c_get_clientdata(client);
	int rc;
#if (DEBUG_LEVEL & DEBUG_LEVEL_FE)
	dev_info(&client->dev, "%s Shutting Down Keyboard And Screen Backlight.\n", __func__);
#endif
	rc =
		regmap_write(bbqX0kbd_data->regmap, REG_BKL, 0);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not write to BBQX0KBD Backlight. Error: %d\n", __func__, rc);
		return rc;
	}

	if (bbqX0kbd_data->class) {
		device_destroy(bbqX0kbd_data->class, 0);
		class_destroy(bbqX0kbd_data->class);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_PM_SLEEP) || IS_ENABLED(CONFIG_DRM_MEDIATEK)
static int bbqX0kbd_suspend(struct device *dev)
{
	int rc;
	struct i2c_client *client = to_i2c_client(dev);
	struct bbqX0kbd_data *bbqX0kbd_data = i2c_get_clientdata(client);

	bbqX0kbd_data->suspended = true;

	/* Turn off backlight to save power */
	rc =
		regmap_write(bbqX0kbd_data->regmap, REG_BKL, 0);
	if (rc != 0) {
		pr_err("%s Could not write to BBQX0KBD Backlight. Error: %d\n", __func__, rc);
		return 0;
	}
	mdelay(10);

	if (device_may_wakeup(dev)) {
		enable_irq_wake(client->irq);
	} else {
		regulator_disable(bbqX0kbd_data->vdd_reg);
	}

	return 0;
}

static int bbqX0kbd_resume(struct device *dev)
{
	int rc;
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
	unsigned int registerValue;
#endif
	struct i2c_client *client = to_i2c_client(dev);
	struct bbqX0kbd_data *bbqX0kbd_data = i2c_get_clientdata(client);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(client->irq);
	} else {
		regulator_enable(bbqX0kbd_data->vdd_reg);

		mdelay(400);

		rc = regmap_write(bbqX0kbd_data->regmap, REG_CFG, REG_CFG_DEFAULT_SETTING);
		if (rc != 0) {
			pr_err("%s Could not write configuration to BBQX0KBD. Error: %d\n",
			       __func__, rc);
		}

#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		rc = regmap_read(bbqX0kbd_data->regmap, REG_CFG, &registerValue);
		if (rc != 0) {
			pr_err("%s Could not read REG_CFG. Error: %d\n",
			       __func__, rc);
		} else {
			pr_info("%s Configuration Register Value: 0x%02X\n",
				__func__, registerValue);
		}
#endif

		rc = regmap_write(bbqX0kbd_data->regmap, REG_CF2, REG_CFG2_DEFAULT_SETTING);
		if (rc != 0) {
			pr_err("%s Could not write configuration 2 to BBQX0KBD. Error: %d\n",
			       __func__, rc);
		}
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		rc = regmap_read(bbqX0kbd_data->regmap, REG_CF2, &registerValue);
		if (rc != 0) {
			pr_err("%s Could not read REG_CF2. Error: %d\n",
			       __func__, rc);
		} else {
			pr_info("%s Configuration 2 Register Value: 0x%02X\n",
				__func__, registerValue);
		}
#endif
	}

	/* Restore backlight level */
	rc = regmap_write(bbqX0kbd_data->regmap, REG_BKL, 255);
	if (rc != 0) {
		pr_err("%s Could not write to BBQX0KBD Backlight. Error: %d\n", __func__, rc);
	}

	bbqX0kbd_data->suspended = false;

	rc = regmap_write(bbqX0kbd_data->regmap, REG_INT, 0);
	if (rc < 0)
		pr_err("%s Could not clear REG_INT. Error: %d\n", __func__, rc);

	return 0;
}

#if !IS_ENABLED(CONFIG_DRM_MEDIATEK)
static SIMPLE_DEV_PM_OPS(bbqX0kbd_pm_ops, bbqX0kbd_suspend, bbqX0kbd_resume);
#endif
#endif

static const struct i2c_device_id bbqX0kbd_i2c_device_id[] = {
	{ "bbqX0kbd", 0, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bbqX0kbd_i2c_device_id);

static const struct of_device_id bbqX0kbd_of_device_id[] = {
	{ .compatible = "wallComputer,bbqX0kbd", },
	{ }
};
MODULE_DEVICE_TABLE(of, bbqX0kbd_of_device_id);

static struct i2c_driver bbqX0kbd_driver = {
	.probe = bbqX0kbd_probe,
	.remove = bbqX0kbd_remove,
	.id_table = bbqX0kbd_i2c_device_id,
	.driver = {
		.name = "bbqX0kbd",
#if IS_ENABLED(CONFIG_PM_SLEEP) && !IS_ENABLED(CONFIG_DRM_MEDIATEK)
		.pm = &bbqX0kbd_pm_ops,
#endif
		.of_match_table = bbqX0kbd_of_device_id,
	},
};

static int __init bbqX0kbd_init(void)
{
	return i2c_add_driver(&bbqX0kbd_driver);
}
module_init(bbqX0kbd_init);

static void __exit bbqX0kbd_exit(void)
{
	pr_info("%s Exiting BBQX0KBD.\n", __func__);
	i2c_del_driver(&bbqX0kbd_driver);
}
module_exit(bbqX0kbd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wallComputer");
MODULE_DESCRIPTION("Keyboard driver for BBQ10, hardware by arturo182 <arturo182@tlen.pl>, software by wallComputer.");
MODULE_VERSION("0.4") ; // To Match software version on SAM-D Controller of arturo182's keyboard featherwing Rev 2.