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

#define LEFT_CTRL_BIT			BIT(0)
#define LEFT_SHIFT_BIT			BIT(1)
#define LEFT_ALT_BIT			BIT(2)
#define RIGHT_CTRL_BIT			BIT(4)
#define RIGHT_SHIFT_BIT			BIT(5)
#define RIGHT_ALT_BIT			BIT(6)

#define NUMS_LOCK_BIT			BIT(1)

struct bbqX0kbd_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct class *class;

	struct regulator *vdd_reg;
	struct regulator *v18_reg;
	struct regulator *v28_reg;
	struct gpio_desc *dfu_gpio;
	struct led_classdev backlight;

	struct work_struct work_struct;
	uint8_t fifoCount;
	uint8_t fifoData[BBQX0KBD_FIFO_SIZE][2];

	uint8_t touchInt;
	int8_t rel_x;
	int8_t rel_y;
	int8_t relX_offset;
	int8_t relY_offset;

	uint8_t version_number;
	uint8_t modifier_keys_status;
	uint8_t lockStatus;
	unsigned short keycode[NUM_KEYCODES];
	struct input_dev *input_dev;

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	struct notifier_block disp_notifier;
#endif

	bool suspended;
	
	uint8_t q20_spec_switch_key_mouse;
};

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
static int bbqX0kbd_suspend(struct device *dev);
static int bbqX0kbd_resume(struct device *dev);
#endif

static uint8_t bbqX0kbd_modkeys_to_bits(unsigned short mod_keycode)
{
	switch (mod_keycode) {
	case KEY_LEFTCTRL: return LEFT_CTRL_BIT;
	case KEY_RIGHTCTRL: return RIGHT_CTRL_BIT;
	case KEY_LEFTSHIFT: return LEFT_SHIFT_BIT;
	case KEY_RIGHTSHIFT: return RIGHT_SHIFT_BIT;
	case KEY_LEFTALT: return LEFT_ALT_BIT;
	case KEY_RIGHTALT: return RIGHT_ALT_BIT;
	}

	return 0;
}

static unsigned short bbqX0kbd_get_num_lock_keycode(unsigned short keycode)
{
	switch (keycode) {
	case KEY_W: return KEY_1;
	case KEY_E: return KEY_2;
	case KEY_R: return KEY_3;
	case KEY_S: return KEY_4;
	case KEY_D: return KEY_5;
	case KEY_F: return KEY_6;
	case KEY_Z: return KEY_7;
	case KEY_X: return KEY_8;
	case KEY_C: return KEY_9;
	case KEY_GRAVE: return KEY_0;
	}

	return keycode;
}

static unsigned short bbqX0kbd_get_altgr_keycode(unsigned short keycode)
{
	switch (keycode) {
	case KEY_E: return KEY_PAGEDOWN;
	case KEY_R: return KEY_PAGEUP;
	case KEY_Y: return KEY_UP;
	case KEY_G: return KEY_LEFT;
	case KEY_H: return KEY_HOME;
	case KEY_J: return KEY_RIGHT;
	case KEY_B: return KEY_DOWN;
	case KEY_M: return KEY_MENU;
	case KEY_K: return KEY_VOLUMEUP;
	case KEY_L: return KEY_VOLUMEDOWN;
	case KEY_GRAVE: return KEY_MUTE;
	case KEY_BACKSPACE: return KEY_DELETE;
	}

	return keycode;
}

static void bbqX0kbd_read_fifo(struct bbqX0kbd_data *bbqX0kbd_data)
{
	struct i2c_client *client = bbqX0kbd_data->client;
	uint8_t fifo_data[2];
	unsigned int count;
	uint8_t pos;
	int rc;

	rc = regmap_read(bbqX0kbd_data->regmap, REG_INT, &count);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not read REG_KEY, Error: %d\n",
			__func__, rc);
		return;
	}
	count = count & REG_KEY_KEYCOUNT_MASK;
	bbqX0kbd_data->fifoCount = count;
	pos = 0;
	while (count) {
		rc = regmap_bulk_read(bbqX0kbd_data->regmap, REG_FIF,
					       fifo_data, sizeof(fifo_data));
		if (rc != 0) {
			dev_err(&client->dev,
				"%s Could not read REG_FIF, Error: %d\n",
				__func__, rc);
			return;
		}
		bbqX0kbd_data->fifoData[pos][0] = fifo_data[0];
		bbqX0kbd_data->fifoData[pos][1] = fifo_data[1];
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		dev_info(
			&client->dev,
			"%s Filled Data: KeyState:%d SCANCODE:%d at Pos: %d Count: %d\n",
			__func__, bbqX0kbd_data->fifoData[pos][0],
			bbqX0kbd_data->fifoData[pos][1], pos, count);
#endif
		++pos;
		--count;
	}
}

static void bbqX0kbd_work_fnc(struct work_struct *work_struct_ptr)
{
	struct bbqX0kbd_data *bbqX0kbd_data = container_of(
		work_struct_ptr, struct bbqX0kbd_data, work_struct);
	struct input_dev *input_dev = bbqX0kbd_data->input_dev;
	struct i2c_client *client = bbqX0kbd_data->client;
	unsigned short keycode;
	uint8_t pos = 0;
	int rc = 0;
	int8_t registerX, registerY;
	unsigned short touch_keycode = 0;

	while (bbqX0kbd_data->fifoCount > 0) {
		if (bbqX0kbd_data->fifoData[pos][0] == KEY_PRESSED_STATE || bbqX0kbd_data->fifoData[pos][0] == KEY_RELEASED_STATE) {
			input_event(input_dev, EV_MSC, MSC_SCAN, bbqX0kbd_data->fifoData[pos][1]);

			keycode = bbqX0kbd_data->keycode[bbqX0kbd_data->fifoData[pos][1]];
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
			dev_info(
				&client->dev,
				"%s BEFORE: MODKEYS: 0x%02X LOCKKEYS: 0x%02X scancode: %d(%c) keycode: %d State: %d\n",
				__func__, bbqX0kbd_data->modifier_keys_status,
				bbqX0kbd_data->lockStatus,
				bbqX0kbd_data->fifoData[pos][1],
				bbqX0kbd_data->fifoData[pos][1], keycode,
				bbqX0kbd_data->fifoData[pos][0]);
#endif
			switch (keycode) {
			case KEY_UNKNOWN:
				dev_warn(
					&client->dev,
					"%s Could not get Keycode for Scancode: [0x%02X]\n",
					__func__,
					bbqX0kbd_data->fifoData[pos][1]);
				break;
			case KEY_RIGHTSHIFT:
				if (bbqX0kbd_data->modifier_keys_status & LEFT_ALT_BIT && bbqX0kbd_data->fifoData[pos][0] == KEY_PRESSED_STATE)
					bbqX0kbd_data->lockStatus ^= NUMS_LOCK_BIT;
				fallthrough;
			case KEY_LEFTSHIFT:
			case KEY_RIGHTALT:
			case KEY_LEFTALT:
			case KEY_LEFTCTRL:
			case KEY_RIGHTCTRL:
				if (bbqX0kbd_data->fifoData[pos][0] == KEY_PRESSED_STATE)
					bbqX0kbd_data->modifier_keys_status |= bbqX0kbd_modkeys_to_bits(keycode);
				else
					bbqX0kbd_data->modifier_keys_status &= ~bbqX0kbd_modkeys_to_bits(keycode);
				fallthrough;
			default:
				if (bbqX0kbd_data->lockStatus & NUMS_LOCK_BIT)
					keycode = bbqX0kbd_get_num_lock_keycode(keycode);
				else if (bbqX0kbd_data->modifier_keys_status & RIGHT_ALT_BIT)
					keycode = bbqX0kbd_get_altgr_keycode(keycode);
				else if (bbqX0kbd_data->modifier_keys_status & LEFT_ALT_BIT && bbqX0kbd_data->fifoData[pos][1] == 0x05 && (bbqX0kbd_data->q20_spec_switch_key_mouse == 0))
					keycode = BTN_RIGHT;
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
				dev_info(
					&client->dev,
					"%s BEFORE: MODKEYS: 0x%02X LOCKKEYS: 0x%02X scancode: %d(%c) keycode: %d State: %d\n",
					__func__,
					bbqX0kbd_data->modifier_keys_status,
					bbqX0kbd_data->lockStatus,
					bbqX0kbd_data->fifoData[pos][1],
					bbqX0kbd_data->fifoData[pos][1],
					keycode,
					bbqX0kbd_data->fifoData[pos][0]);
#endif
				input_report_key(input_dev, keycode, bbqX0kbd_data->fifoData[pos][0] == KEY_PRESSED_STATE);
				break;
			}
		}
		++pos;
		--bbqX0kbd_data->fifoCount;
	}

	if (bbqX0kbd_data->touchInt) {
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		dev_info(&client->dev, "%s X Reg: %d Y Reg: %d.\n", __func__,
			 bbqX0kbd_data->rel_x, bbqX0kbd_data->rel_y);
#endif
		registerX = bbqX0kbd_data->rel_x;
		registerY = bbqX0kbd_data->rel_y;
		if (bbqX0kbd_data->q20_spec_switch_key_mouse == 0) {
			input_report_rel(input_dev, REL_X, (int8_t)registerX);
			input_report_rel(input_dev, REL_Y, (int8_t)registerY);
		} else if (bbqX0kbd_data->q20_spec_switch_key_mouse == 1){
			bbqX0kbd_data->relX_offset += registerX;
			bbqX0kbd_data->relY_offset += registerY;
	
			pr_info("%s:relX_offset= %d; relY_offset= %d\n",__func__,bbqX0kbd_data->relX_offset,bbqX0kbd_data->relY_offset);
			// 判断滑动方向
			if (abs(bbqX0kbd_data->relY_offset) > BBQ20KBD_TRACKPAD_KEYS_OFFSET) {// 垂直滑动
				pr_info("------>registerY = %d\n",bbqX0kbd_data->relY_offset);
				if (bbqX0kbd_data->relY_offset > 0) {
					touch_keycode = KEY_DOWN; //0x6c
				} else {
					touch_keycode = KEY_UP;  //0x67
				}
			}
			
			if(abs(bbqX0kbd_data->relX_offset) > BBQ20KBD_TRACKPAD_KEYS_OFFSET) {// 水平滑动
				pr_info("------>registerX = %d\n",bbqX0kbd_data->relX_offset);
				if (bbqX0kbd_data->relX_offset > 0) {
					touch_keycode = KEY_RIGHT;  //0x6a
				} else {
					touch_keycode = KEY_LEFT; //0x69
				}
			}
			
			if(touch_keycode > 0) {
				pr_info("------>key code = %x\n",touch_keycode);
				input_report_key(input_dev, touch_keycode, 1);  // 按下
				input_report_key(input_dev, touch_keycode, 0);  // 释放
				bbqX0kbd_data->relY_offset = 0;
				bbqX0kbd_data->relX_offset = 0;
			}
		}
		
		bbqX0kbd_data->touchInt = 0;
	} else {
		bbqX0kbd_data->relX_offset = 0;
		bbqX0kbd_data->relY_offset = 0;
	}

	input_sync(input_dev);
	regmap_write(bbqX0kbd_data->regmap, REG_INT, 0);
	if (rc < 0)
		dev_err(&client->dev, "%s Could not clear REG_INT. Error: %d\n",
			__func__, rc);
	enable_irq(client->irq);
}

static irqreturn_t bbqX0kbd_irq_handler(int irq, void *dev_id)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_id;
	struct i2c_client *client = bbqX0kbd_data->client;
	int rc;
	unsigned int registerValue;

#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
	dev_info(&client->dev, "%s Interrupt Fired. IRQ: %d\n", __func__, irq);
#endif
	rc =
		regmap_read(bbqX0kbd_data->regmap, REG_INT, &registerValue);
	if (rc < 0) {
		dev_err(&client->dev, "%s: Could not read REG_INT. Error: %d\n", __func__, rc);
		return IRQ_NONE;
	}

#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
	dev_info(&client->dev, "%s Interrupt: 0x%02x\n", __func__, registerValue);
#endif

	if (registerValue == 0x00) {
		return IRQ_NONE;
	}

	disable_irq_nosync(client->irq);

	if (registerValue & REG_INT_OVERFLOW)
		dev_warn(&client->dev, "%s overflow occurred.\n", __func__);

	if (registerValue & REG_INT_KEY) {
		bbqX0kbd_read_fifo(bbqX0kbd_data);
		schedule_work(&bbqX0kbd_data->work_struct);

		if(bbqX0kbd_data->q20_spec_switch_key_mouse == 1) {
			return IRQ_HANDLED;
		}
	}

	if (registerValue & REG_INT_TOUCH) {
		rc = regmap_read(bbqX0kbd_data->regmap, REG_TOX,
					  &registerValue);
		if (rc < 0) {
			dev_err(&client->dev, "%s : Could not read REG_TOX. Error: %d\n", __func__, rc);
			enable_irq(client->irq);
			return IRQ_NONE;
		}
		bbqX0kbd_data->rel_x = (int8_t)registerValue;
		rc = regmap_read(bbqX0kbd_data->regmap, REG_TOY,
					  &registerValue);
		if (rc < 0) {
			dev_err(&client->dev, "%s : Could not read REG_TOY. Error: %d\n", __func__, rc);
			enable_irq(client->irq);
			return IRQ_NONE;
		}
		bbqX0kbd_data->rel_y = (int8_t)registerValue;
		bbqX0kbd_data->touchInt = 1;
		schedule_work(&bbqX0kbd_data->work_struct);
	} else
		bbqX0kbd_data->touchInt = 0;

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
		//trun off
		regulator_disable(bbqX0kbd_data->vdd_reg);

		if (bbqX0kbd_data->dfu_gpio >= 0) {
			gpiod_set_value_cansleep(bbqX0kbd_data->dfu_gpio, 0);
		}
		mdelay(100);

		regulator_enable(bbqX0kbd_data->vdd_reg);
	} else if (buf[0] == '1') {
		//trun off
		regulator_disable(bbqX0kbd_data->vdd_reg);

	} else if (buf[0] == '2') {
		//firmware upgrade
		if (bbqX0kbd_data->dfu_gpio >= 0) {
			gpiod_set_value_cansleep(bbqX0kbd_data->dfu_gpio, 1);
		}

		regulator_enable(bbqX0kbd_data->vdd_reg);
	}

	return count;
}

static DEVICE_ATTR(firmware_upgrade, 0644, get_firmware_upgrade, set_firmware_upgrade);

static ssize_t get_switch_key_mouse(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n",
			  bbqX0kbd_data->q20_spec_switch_key_mouse);
}

static ssize_t set_switch_key_mouse(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct bbqX0kbd_data *bbqX0kbd_data = dev_get_drvdata(dev);

	if (buf[0] == '0') {
		bbqX0kbd_data->q20_spec_switch_key_mouse = 0;
		bbqX0kbd_data->keycode[0x05] = BTN_LEFT;
	} else if (buf[0] == '1') {
		bbqX0kbd_data->q20_spec_switch_key_mouse = 1;
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

	rc =
		regmap_write(bbqX0kbd_data->regmap, REG_RST, 0);
	if (rc) {
		dev_err(&client->dev, "%s Could not Reset BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
	msleep(400);

	rc =
		regmap_read(bbqX0kbd_data->regmap, REG_VER, &registerValue);
	if (rc != 0) {
		dev_err(&client->dev, "%s Could not Read Version BBQX0KBD. Error: %d\n", __func__, rc);
		return -ENODEV;
	}
	dev_info(&client->dev, "%s BBQX0KBD Software version: 0x%02X\n", __func__, registerValue);
	bbqX0kbd_data->version_number = registerValue;

	rc = regmap_write(bbqX0kbd_data->regmap, REG_CFG,
				   REG_CFG_DEFAULT_SETTING);
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

	rc = regmap_write(bbqX0kbd_data->regmap, REG_CF2,
				   REG_CFG2_DEFAULT_SETTING);
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
	bbqX0kbd_data->q20_spec_switch_key_mouse = 0;
	bbqX0kbd_data->keycode[0x05] = BTN_LEFT;
#elif (BBQ20KBD_TRACKPAD_USE == BBQ20KBD_TRACKPAD_AS_KEYS)
	bbqX0kbd_data->q20_spec_switch_key_mouse = 1;
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

	bbqX0kbd_data->modifier_keys_status = 0x00; // Serendipitously coincides with idle state of all keys.
	bbqX0kbd_data->lockStatus = 0x00;
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

	rc = devm_request_threaded_irq(dev, client->irq,
										NULL, bbqX0kbd_irq_handler,
										IRQF_SHARED | IRQF_ONESHOT,
										client->name, bbqX0kbd_data);

	if (rc != 0) {
		dev_err(&client->dev, "Coudl not claim IRQ %d; error %d\n", client->irq, rc);
		return rc;
	}
	INIT_WORK(&bbqX0kbd_data->work_struct, bbqX0kbd_work_fnc);

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
		disable_irq(client->irq);
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

		rc = regmap_write(bbqX0kbd_data->regmap, REG_CFG,
					   REG_CFG_DEFAULT_SETTING);
		if (rc != 0) {
			pr_err("%s Could not write configuration to BBQX0KBD. Error: %d\n",
			       __func__, rc);
		}

#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		rc = regmap_read(bbqX0kbd_data->regmap, REG_CFG,
					  &registerValue);
		if (rc != 0) {
			pr_err("%s Could not read REG_CFG. Error: %d\n",
			       __func__, rc);
		} else {
			pr_info("%s Configuration Register Value: 0x%02X\n",
				__func__, registerValue);
		}
#endif

		rc = regmap_write(bbqX0kbd_data->regmap, REG_CF2,
					   REG_CFG2_DEFAULT_SETTING);
		if (rc != 0) {
			pr_err("%s Could not write configuration 2 to BBQX0KBD. Error: %d\n",
			       __func__, rc);
		}
#if (DEBUG_LEVEL & DEBUG_LEVEL_LD)
		rc = regmap_read(bbqX0kbd_data->regmap, REG_CF2,
					  &registerValue);
		if (rc != 0) {
			pr_err("%s Could not read REG_CF2. Error: %d\n",
			       __func__, rc);
		} else {
			pr_info("%s Configuration 2 Register Value: 0x%02X\n",
				__func__, registerValue);
		}
#endif

		enable_irq(client->irq);
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