// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021-2021 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[VIRTUAL_BUCK]([%s][%d]): " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/mutex.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/list.h>
#include <soc/oplus/system/boot_mode.h>
#include <soc/oplus/device_info.h>
#include <soc/oplus/system/oplus_project.h>
#include "oplus_chg_module.h"
#include "oplus_chg_ic.h"
#include "../oplus_debug_info.h"
#include "../charger_ic/oplus_virtual_charger.h"
#include "../charger_ic/op_charge.h"

struct oplus_virtual_buck_child {
	struct oplus_chg_ic_dev *ic_dev;
	int index;
	int current_ratio;
	enum oplus_chg_ic_func *funcs;
	int func_num;
	enum oplus_chg_ic_virq_id *virqs;
	int virq_num;
};

struct oplus_vc_misc_gpio {
	struct pinctrl		*pinctrl;
	struct mutex		pinctrl_mutex;

	int			ship_gpio;
	struct pinctrl_state	*ship_active;
	struct pinctrl_state	*ship_sleep;

	int			vchg_trig_gpio;
	struct pinctrl_state	*vchg_trig_default;

	int			ccdetect_gpio;
	struct pinctrl_state	*ccdetect_active;
	struct pinctrl_state	*ccdetect_sleep;

	int dischg_gpio;
	struct pinctrl_state *dischg_enable;
	struct pinctrl_state *dischg_disable;
};

struct oplus_virtual_buck_ic {
	struct device *dev;
	struct oplus_chg_ic_dev *ic_dev;
	bool online;
	enum oplus_chg_ic_connect_type connect_type;
	int child_num;
	struct oplus_virtual_buck_child *child_list;

	/* parallel charge */
	int main_charger;
	int aux_charger_en_thr_ma;

	struct iio_channel *usbtemp_v_chan;
	struct iio_channel *usbtemp_sup_v_chan;
	struct adc_vol_temp_info *adc_vol_temp_info;

	struct oplus_vc_misc_gpio misc_gpio;

	int ccdetect_irq;

	bool otg_switch;
};

static int oplus_chg_vb_set_typec_mode(struct oplus_chg_ic_dev *ic_dev,
				       enum oplus_chg_typec_port_role_type mode);
static int oplus_vb_virq_register(struct oplus_virtual_buck_ic *chip);

static inline bool func_is_support(struct oplus_virtual_buck_child *ic,
				   enum oplus_chg_ic_func func_id)
{
	if (ic->func_num > 0)
		return oplus_chg_ic_func_is_support(ic->funcs, ic->func_num, func_id);
	else
		return false;
}

static inline bool virq_is_support(struct oplus_virtual_buck_child *ic,
				   enum oplus_chg_ic_virq_id virq_id)
{
	if (ic->virq_num > 0)
		return oplus_chg_ic_virq_is_support(ic->virqs, ic->virq_num, virq_id);
	else
		return false;
}

irqreturn_t oplus_vc_ccdetect_change_handler(int irq, void *data)
{
	struct oplus_virtual_buck_ic *chip = data;

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_CC_DETECT);
	return IRQ_HANDLED;
}

static int oplus_vc_usbtemp_l_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	struct pinctrl_state *usbtemp_l_gpio_default = NULL;
	int rc;

	chip->misc_gpio.pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(chip->misc_gpio.pinctrl)) {
		chg_err("get pinctrl fail\n");
		return -EINVAL;
	}

	usbtemp_l_gpio_default = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "usbtemp_l_gpio_default");
	if (IS_ERR_OR_NULL(usbtemp_l_gpio_default)) {
		chg_err("get usbtemp_l_gpio_default error\n");
		return -EINVAL;
	}

	rc = pinctrl_select_state(chip->misc_gpio.pinctrl, usbtemp_l_gpio_default);
	if (rc < 0)
		chg_err("set usbtemp_l_gpio to default error\n");

	return rc;
}

static int oplus_vc_usbtemp_r_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	struct pinctrl_state *usbtemp_r_gpio_default = NULL;
	int rc;

	chip->misc_gpio.pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(chip->misc_gpio.pinctrl)) {
		chg_err("get pinctrl fail\n");
		return -EINVAL;
	}

	usbtemp_r_gpio_default = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "usbtemp_r_gpio_default");
	if (IS_ERR_OR_NULL(usbtemp_r_gpio_default)) {
		chg_err("get usbtemp_r_gpio_default error\n");
		return -EINVAL;
	}

	rc = pinctrl_select_state(chip->misc_gpio.pinctrl, usbtemp_r_gpio_default);
	if (rc < 0)
		chg_err("set usbtemp_r_gpio to default error\n");

	return rc;
}

static bool oplus_vc_usbtemp_check_is_support(struct oplus_virtual_buck_ic *chip)
{
	if (get_eng_version() == AGING) {
		chg_err("AGING mode, disable usbtemp\n");
		return false;
	}

	if(gpio_is_valid(chip->misc_gpio.dischg_gpio))
		return true;

	chg_info("dischg return false\n");

	return false;
}

static int oplus_vc_dischg_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	int rc;

	chip->misc_gpio.pinctrl = devm_pinctrl_get(chip->dev);

	if (IS_ERR_OR_NULL(chip->misc_gpio.pinctrl)) {
		chg_err("get dischg_pinctrl fail\n");
		return -EINVAL;
	}

	chip->misc_gpio.dischg_enable = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "dischg_enable");
	if (IS_ERR_OR_NULL(chip->misc_gpio.dischg_enable)) {
		chg_err("get dischg_enable fail\n");
		return -EINVAL;
	}

	chip->misc_gpio.dischg_disable = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "dischg_disable");
	if (IS_ERR_OR_NULL(chip->misc_gpio.dischg_disable)) {
		chg_err("get dischg_disable fail\n");
		return -EINVAL;
	}

	rc = pinctrl_select_state(chip->misc_gpio.pinctrl, chip->misc_gpio.dischg_disable);
	if (rc < 0)
		chg_err("set dischg disable error\n");

	return rc;
}

static int oplus_vc_usbtemp_adc_init(struct oplus_virtual_buck_ic *chip)
{
	struct device_node *node = chip->dev->of_node;
	const char *name;
	int i;
	int rc = 0;

	chip->misc_gpio.dischg_gpio = of_get_named_gpio(node, "oplus,dischg-gpio", 0);
	if (!gpio_is_valid(chip->misc_gpio.dischg_gpio)) {
		chg_info("Couldn't read oplus,dischg-gpio, rc=%d\n",
			chip->misc_gpio.dischg_gpio);
	} else {
		if (oplus_vc_usbtemp_check_is_support(chip) == true) {
			rc = gpio_request(chip->misc_gpio.dischg_gpio, "dischg-gpio");
			if (rc) {
				chg_err("unable to request dischg-gpio:%d\n", chip->misc_gpio.dischg_gpio);
				return rc;
			}
			rc = oplus_vc_dischg_gpio_init(chip);
			if (rc) {
				chg_err("unable to init dischg-gpio:%d\n", chip->misc_gpio.dischg_gpio);
				goto free_dischg_gpio;
			}
		}
	}

	rc = oplus_vc_usbtemp_l_gpio_init(chip);
	if (rc < 0) {
		chg_err("usbtemp_l_gpio init error, rc=%d\n", rc);
		goto free_dischg_gpio;
	}
	oplus_vc_usbtemp_r_gpio_init(chip);
	if (rc < 0) {
		chg_err("usbtemp_r_gpio init error, rc=%d\n", rc);
		goto free_dischg_gpio;
	}

	rc = of_property_read_string(node, "oplus,adc_info_name", &name);
	if (rc < 0) {
		chg_err("read oplus,adc_info_name error, rc=%d\n", rc);
		goto free_dischg_gpio;
	}
	for (i = 0; i < ARRAY_SIZE(adc_vol_temp_info_table); i++) {
		if (strcmp(adc_vol_temp_info_table[i].name, name) == 0) {
			chip->adc_vol_temp_info = &adc_vol_temp_info_table[i];
		}
	}
	if (chip->adc_vol_temp_info == NULL) {
		chg_err("%s adc_vol_temp_info not found\n", name);
		chip->adc_vol_temp_info = &adc_vol_temp_info_table[0];
	}

	return 0;

free_dischg_gpio:
	if (oplus_vc_usbtemp_check_is_support(chip))
		gpio_free(chip->misc_gpio.dischg_gpio);
	return rc;
}

static int oplus_vc_usbtemp_iio_init(struct oplus_virtual_buck_ic *chip)
{
	struct device_node *node = chip->dev->of_node;
	int rc = 0;

	rc = of_property_match_string(node, "io-channel-names", "usb_temp_adc");
	if (rc >= 0) {
		chip->usbtemp_v_chan = iio_channel_get(chip->dev, "usb_temp_adc");
		if (IS_ERR(chip->usbtemp_v_chan)) {
			rc = PTR_ERR(chip->usbtemp_v_chan);
			chg_err("usb_temp_adc get error, rc=%d\n", rc);
			chip->usbtemp_v_chan = NULL;
			return rc;
		}
	} else {
		chg_err("usb_temp_adc not found\n");
	}

	rc = of_property_match_string(node, "io-channel-names", "usb_supplementary_temp_adc");
	if (rc >= 0) {
		chip->usbtemp_sup_v_chan = iio_channel_get(chip->dev, "usb_supplementary_temp_adc");
		if (IS_ERR(chip->usbtemp_sup_v_chan)) {
			rc = PTR_ERR(chip->usbtemp_sup_v_chan);
			chg_err("usb_supplementary_temp_adc get error, rc=%d\n", rc);
			chip->usbtemp_sup_v_chan = NULL;
			return rc;
		}
	} else {
		chg_err("usb_supplementary_temp_adc not found\n");
	}

	return 0;
}

static int oplus_vc_ship_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	int rc;

	chip->misc_gpio.pinctrl = devm_pinctrl_get(chip->dev);

	chip->misc_gpio.ship_active = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "ship_active");
	if (IS_ERR_OR_NULL(chip->misc_gpio.ship_active)) {
		chg_err("get ship_active fail\n");
		return -EINVAL;
	}

	chip->misc_gpio.ship_sleep = pinctrl_lookup_state(chip->misc_gpio.pinctrl, "ship_sleep");
	if (IS_ERR_OR_NULL(chip->misc_gpio.ship_sleep)) {
		chg_err("get ship_sleep fail\n");
		return -EINVAL;
	}

	rc = pinctrl_select_state(chip->misc_gpio.pinctrl, chip->misc_gpio.ship_sleep);
	if (rc < 0)
		chg_err("set ship gpio sleep error, rc=%d\n", rc);

	return rc;
}

int oplus_vc_ccdetect_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	int rc;

	chip->misc_gpio.ccdetect_active = pinctrl_lookup_state(
		chip->misc_gpio.pinctrl, "ccdetect_active");
	if (IS_ERR_OR_NULL(chip->misc_gpio.ccdetect_active)) {
		chg_err("get ccdetect_active fail\n");
		return -EINVAL;
	}

	chip->misc_gpio.ccdetect_sleep = pinctrl_lookup_state(
		chip->misc_gpio.pinctrl, "ccdetect_sleep");
	if (IS_ERR_OR_NULL(chip->misc_gpio.ccdetect_sleep)) {
		chg_err("get ccdetect_sleep fail\n");
		return -EINVAL;
	}

	if (gpio_is_valid(chip->misc_gpio.ccdetect_gpio)) {
		gpio_direction_input(chip->misc_gpio.ccdetect_gpio);
	}

	pinctrl_select_state(chip->misc_gpio.pinctrl,
		chip->misc_gpio.ccdetect_active);
	rc = pinctrl_select_state(chip->misc_gpio.pinctrl, chip->misc_gpio.ccdetect_active);
	if (rc < 0)
		chg_err("set ccdetect gpio active error, rc=%d\n", rc);

	return rc;
}

bool oplus_vc_ccdetect_gpio_support(struct oplus_virtual_buck_ic *chip)
{
	int boot_mode = get_boot_mode();

	/* HW engineer requirement */
	if (boot_mode == MSM_BOOT_MODE__RF ||
	    boot_mode == MSM_BOOT_MODE__WLAN ||
	    boot_mode == MSM_BOOT_MODE__FACTORY)
		return false;

	if (gpio_is_valid(chip->misc_gpio.ccdetect_gpio))
		return true;

	return false;
}

static int oplus_ccdetect_enable(struct oplus_virtual_buck_ic *chip, bool en)
{
	int rc;

	if (!oplus_vc_ccdetect_gpio_support(chip))
		return 0;

	rc = oplus_chg_vb_set_typec_mode(chip->ic_dev,
		en ? TYPEC_PORT_ROLE_DRP : TYPEC_PORT_ROLE_DISABLE);
	if (rc < 0)
		chg_err("%s ccdetect error, rc=%d\n", en ? "enable" : "disable", rc);

	return rc;
}

static int oplus_vc_ccdetect_before_irq_register(struct oplus_virtual_buck_ic *chip)
{
	int level = 1;

	level = gpio_get_value(chip->misc_gpio.ccdetect_gpio);
	usleep_range(2000, 2100);
	if (level != gpio_get_value(chip->misc_gpio.ccdetect_gpio)) {
		chg_err("ccdetect_gpio is unstable, try again...\n");
		usleep_range(10000, 11000);
		level = gpio_get_value(chip->misc_gpio.ccdetect_gpio);
	}

	if (level <= 0) {
		oplus_ccdetect_enable(chip, true);
	}

	return 0;
}

static int oplus_vc_ccdetect_irq_register(struct oplus_virtual_buck_ic *chip)
{
	int rc = 0;

	rc = devm_request_threaded_irq(chip->dev, chip->ccdetect_irq,
			NULL, oplus_vc_ccdetect_change_handler, IRQF_TRIGGER_FALLING
			| IRQF_TRIGGER_RISING | IRQF_ONESHOT, "ccdetect-change", chip);
	if (rc < 0) {
		chg_err("Unable to request ccdetect-change irq: %d\n", rc);
		return rc;
	}

	rc = enable_irq_wake(chip->ccdetect_irq);
	if (rc != 0) {
		chg_err("enable_irq_wake: ccdetect_irq failed %d\n", rc);
		devm_free_irq(chip->dev, chip->ccdetect_irq, chip);
	}

	return rc;
}

static int oplus_vc_input_pg_gpio_init(struct oplus_virtual_buck_ic *chip)
{
	struct pinctrl *input_pg_pinctrl = NULL;
	struct pinctrl_state *input_pg_default = NULL;

	input_pg_pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(input_pg_pinctrl)) {
		chg_err("get input_pg_pinctrl fail\n");
		return -EINVAL;
	}

	input_pg_default = pinctrl_lookup_state(input_pg_pinctrl, "input_pg_default");
	if (IS_ERR_OR_NULL(input_pg_default)) {
		chg_err("get input_pg_default fail\n");
		return -EINVAL;
	}

	pinctrl_select_state(input_pg_pinctrl, input_pg_default);

	return 0;
}

#if defined(OPLUS_FEATURE_POWERINFO_FTM) && defined(CONFIG_OPLUS_POWERINFO_FTM)
extern bool ext_boot_with_console(void);
#endif
static int oplus_vc_chg_2uart_pinctrl_init(struct oplus_virtual_buck_ic *chip)
{
	struct pinctrl		*chg_2uart_pinctrl;
	struct pinctrl_state	*chg_2uart_active;
	struct pinctrl_state	*chg_2uart_sleep;

	chg_2uart_pinctrl = devm_pinctrl_get(chip->dev);

	if (IS_ERR_OR_NULL(chg_2uart_pinctrl)) {
		chg_err("get 2uart chg_2uart_pinctrl fail\n");
		return -EINVAL;
	}

	chg_2uart_active = pinctrl_lookup_state(chg_2uart_pinctrl, "chg_qupv3_se3_2uart_active");
	if (IS_ERR_OR_NULL(chg_2uart_active)) {
		chg_err("get chg_qupv3_se3_2uart_active fail\n");
		return -EINVAL;
	}

	chg_2uart_sleep = pinctrl_lookup_state(chg_2uart_pinctrl, "chg_qupv3_se3_2uart_sleep");
	if (IS_ERR_OR_NULL(chg_2uart_sleep)) {
		chg_err("get chg_qupv3_se3_2uart_sleep fail\n");
		return -EINVAL;
	}

#if defined(OPLUS_FEATURE_POWERINFO_FTM) && defined(CONFIG_OPLUS_POWERINFO_FTM)
	if (!ext_boot_with_console()) {
		chg_err("set chg_qupv3_se3_2uart_sleep\n");
		pinctrl_select_state(chg_2uart_pinctrl, chg_2uart_sleep);
	}
#endif

	return 0;
}

static int oplus_vc_misc_init(struct oplus_virtual_buck_ic *chip)
{
	struct device_node *node = chip->dev->of_node;
	int rc = 0;

	chip->misc_gpio.ship_gpio = of_get_named_gpio(node, "oplus,ship-gpio", 0);
	if (!gpio_is_valid(chip->misc_gpio.ship_gpio)) {
		chg_err("Couldn't read oplus,ship-gpio, rc=%d\n",
			chip->misc_gpio.ship_gpio);
	} else {
		rc = gpio_request(chip->misc_gpio.ship_gpio, "ship-gpio");
		if (rc) {
			chg_err("unable to request ship-gpio:%d\n", chip->misc_gpio.ship_gpio);
			return rc;
		}
		rc = oplus_vc_ship_gpio_init(chip);
		if (rc) {
			chg_err("unable to init ship-gpio:%d\n", chip->misc_gpio.ship_gpio);
			goto free_ship_gpio;
		}
		chg_info("init ship-gpio level[%d]\n", gpio_get_value(chip->misc_gpio.ship_gpio));
	}

#if 0 //nick.hu TODO
	chip->misc_gpio.vchg_trig_gpio = of_get_named_gpio(node, "oplus,vchg_trig-gpio", 0);
	if (!gpio_is_valid(chip->misc_gpio.vchg_trig_gpio)) {
		chg_err("Couldn't read oplus,vchg_trig_gpio, rc=%d\n",
			chip->misc_gpio.vchg_trig_gpio);
	} else {
		rc = gpio_request(chip->misc_gpio.vchg_trig_gpio, "vchg_trig-gpio");
		if (rc) {
			chg_err("unable to vchg_trig-gpio:%d\n",
				chip->misc_gpio.vchg_trig_gpio);
			goto free_ship_gpio;
		} else {
			rc = oplus_vc_vchg_trig_gpio_init(chip);
			if (rc) {
				chg_err("unable to init vchg_trig-gpio:%d\n",
						chip->misc_gpio.vchg_trig_gpio);
				goto free_vchg_trig_gpio;
			}
			oplus_vchg_trig_irq_init(chip);
		}
	}
#endif

	chip->misc_gpio.ccdetect_gpio = of_get_named_gpio(node, "oplus,ccdetect-gpio", 0);
	if (!gpio_is_valid(chip->misc_gpio.ccdetect_gpio)) {
		chg_err("Couldn't read oplus,ccdetect-gpio, rc=%d\n",
			chip->misc_gpio.ccdetect_gpio);
	} else {
		if (oplus_vc_ccdetect_gpio_support(chip)) {
			rc = gpio_request(chip->misc_gpio.ccdetect_gpio, "ccdetect-gpio");
			if (rc) {
				chg_err("unable to request ccdetect-gpio:%d\n", chip->misc_gpio.ccdetect_gpio);
				goto free_vchg_trig_gpio;
			} else {
				rc = oplus_vc_ccdetect_gpio_init(chip);
				if (rc) {
					chg_err("unable to init ccdetect-gpio:%d\n", chip->misc_gpio.ccdetect_gpio);
					goto free_ccdetect_gpio;
				}
				chip->ccdetect_irq = gpio_to_irq(chip->misc_gpio.ccdetect_gpio);
			}
		}
	}

	oplus_vc_input_pg_gpio_init(chip);
	oplus_vc_chg_2uart_pinctrl_init(chip);

free_ccdetect_gpio:
	if (oplus_vc_ccdetect_gpio_support(chip))
		gpio_free(chip->misc_gpio.ccdetect_gpio);
free_vchg_trig_gpio:
	if (gpio_is_valid(chip->misc_gpio.vchg_trig_gpio))
		gpio_free(chip->misc_gpio.vchg_trig_gpio);
free_ship_gpio:
	if (gpio_is_valid(chip->misc_gpio.ship_gpio))
		gpio_free(chip->misc_gpio.ship_gpio);

	return rc;
}

static int oplus_vc_child_funcs_init(struct oplus_virtual_buck_ic *chip, int child_num)
{
	struct device_node *node = chip->dev->of_node;
	struct device_node *func_node = NULL;
	int i, m;
	int rc = 0;

	for (i = 0; i < child_num; i++) {
		func_node = of_parse_phandle(node, "oplus,buck_ic_func_group", i);
		if (func_node == NULL) {
			chg_err("can't get ic[%d] function group\n", i);
			rc = -ENODATA;
			goto err;
		}
		rc = of_property_count_elems_of_size(func_node, "functions",
						     sizeof(u32));
		if (rc < 0) {
			chg_err("can't get ic[%d] functions size, rc=%d\n", i, rc);
			goto err;
		}
		chip->child_list[i].func_num = rc;
		chip->child_list[i].funcs =
			devm_kzalloc(chip->dev,
				     sizeof(enum oplus_chg_ic_func) *
					     chip->child_list[i].func_num,
				     GFP_KERNEL);
		if (chip->child_list[i].funcs == NULL) {
			rc = -ENOMEM;
			chg_err("alloc child ic funcs memory error\n");
			goto err;
		}
		rc = of_property_read_u32_array(
			func_node, "functions",
			(u32 *)chip->child_list[i].funcs,
			chip->child_list[i].func_num);
		if (rc) {
			i++;
			chg_err("can't get ic[%d] functions, rc=%d\n", i, rc);
			goto err;
		}
		(void)oplus_chg_ic_func_table_sort(
			chip->child_list[i].funcs,
			chip->child_list[i].func_num);
	}

	return 0;

err:
	for (m = i; m > 0; m--)
		devm_kfree(chip->dev, chip->child_list[m - 1].funcs);
	return rc;
}

static int oplus_vc_child_virqs_init(struct oplus_virtual_buck_ic *chip, int child_num)
{
	struct device_node *node = chip->dev->of_node;
	struct device_node *virq_node = NULL;
	int i, m;
	int rc = 0;

	for (i = 0; i < child_num; i++) {
		virq_node = of_parse_phandle(node, "oplus,buck_ic_func_group", i);
		if (virq_node == NULL) {
			chg_err("can't get ic[%d] function group\n", i);
			rc = -ENODATA;
			goto err;
		}
		rc = of_property_count_elems_of_size(virq_node, "virqs",
						     sizeof(u32));
		if (rc <= 0) {
			chip->child_list[i].virq_num = 0;
			chip->child_list[i].virqs = NULL;
			continue;
		}
		chip->child_list[i].virq_num = rc;
		chip->child_list[i].virqs =
			devm_kzalloc(chip->dev,
				     sizeof(enum oplus_chg_ic_func) *
					     chip->child_list[i].virq_num,
				     GFP_KERNEL);
		if (chip->child_list[i].virqs == NULL) {
			rc = -ENOMEM;
			chg_err("alloc child ic virqs memory error\n");
			goto err;
		}
		rc = of_property_read_u32_array(
			virq_node, "virqs",
			(u32 *)chip->child_list[i].virqs,
			chip->child_list[i].virq_num);
		if (rc) {
			i++;
			chg_err("can't get ic[%d] virqs, rc=%d\n", i, rc);
			goto err;
		}
	}

	return 0;

err:
	for (m = i; m > 0; m--) {
		if (chip->child_list[m - 1].virqs != NULL)
			devm_kfree(chip->dev, chip->child_list[m - 1].virqs);
	}
	return rc;
}

static int oplus_vc_child_init(struct oplus_virtual_buck_ic *chip)
{
	struct device_node *node = chip->dev->of_node;
	int i;
	int rc = 0;

	rc = of_property_read_u32(node, "oplus,buck_ic_connect",
				  &chip->connect_type);
	if (rc < 0) {
		chg_err("can't get buck ic connect type, rc=%d\n", rc);
		return rc;
	}
	rc = of_property_read_u32(node, "oplus,main_charger", &chip->main_charger);
	if (rc < 0) {
		chg_err("can't get main charger index, rc=%d\n", rc);
		return rc;
	}
	rc = of_property_count_elems_of_size(node, "oplus,buck_ic",
					     sizeof(u32));
	if (rc < 0) {
		chg_err("can't get buck ic number, rc=%d\n", rc);
		return rc;
	}
	chip->child_num = rc;
	chip->child_list = devm_kzalloc(
		chip->dev,
		sizeof(struct oplus_virtual_buck_child) * chip->child_num,
		GFP_KERNEL);
	if (chip->child_list == NULL) {
		rc = -ENOMEM;
		chg_err("alloc child ic memory error\n");
		return rc;
	}

	for (i = 0; i < chip->child_num; i++) {
		chip->child_list[i].ic_dev = of_get_oplus_chg_ic(node, "oplus,buck_ic", i);
		if (chip->child_list[i].ic_dev == NULL) {
			chg_err("not find buck ic %d\n", i);
			rc = -EAGAIN;
			goto read_property_err;
		}
		rc = of_property_read_u32_index(
			node, "oplus,buck_ic_current_ratio", i,
			&chip->child_list[i].current_ratio);
		if (rc < 0) {
			chg_err("can't read ic[%d] current ratio, rc=%d\n", i,
			       rc);
			goto read_property_err;
		}
	}

	rc = oplus_vc_child_funcs_init(chip, chip->child_num);
	if (rc < 0)
		goto child_funcs_init_err;
	rc = oplus_vc_child_virqs_init(chip, chip->child_num);
	if (rc < 0)
		goto child_virqs_init_err;

	return 0;

child_virqs_init_err:
	for (i = 0; i < chip->child_num; i++)
		devm_kfree(chip->dev, chip->child_list[i].funcs);
child_funcs_init_err:
read_property_err:
	for (; i >=0; i--)
		chip->child_list[i].ic_dev = NULL;
	devm_kfree(chip->dev, chip->child_list);
	return rc;
}

static int oplus_chg_vb_init(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *chip;
	int i, m;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	chip = oplus_chg_ic_get_drvdata(ic_dev);

	rc = oplus_vc_child_init(chip);
	if (rc < 0) {
		chg_err("child list init error, rc=%d\n", rc);
		goto child_list_init_err;
	}

	rc = oplus_vb_virq_register(chip);
	if (rc < 0) {
		chg_err("virq register error, rc=%d\n", rc);
		goto virq_register_err;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < chip->child_num; i++) {
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_INIT);
		if (rc < 0) {
			chg_err("child ic[%d] init error, rc=%d\n", i, rc);
			goto child_init_err;
		}
		chip->child_list[i].ic_dev->parent = ic_dev;
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_BUCK_SET_ICL) ||
		    chip->child_list[i].current_ratio == 0) {
			oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, true);
		}
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_BUCK_SET_FCC) ||
		    chip->child_list[i].current_ratio == 0) {
			oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, true);
		}
	}

	if (oplus_vc_ccdetect_gpio_support(chip)) {
		rc = oplus_vc_ccdetect_before_irq_register(chip);
		if (rc < 0)
			goto child_init_err;
		rc = oplus_vc_ccdetect_irq_register(chip);
		if (rc < 0)
			goto child_init_err;
	}
	ic_dev->online = true;

	return 0;

child_init_err:
	for (m = i + 1; m > 0; m--)
		oplus_chg_ic_func(chip->child_list[m - 1].ic_dev, OPLUS_IC_FUNC_EXIT);
virq_register_err:
child_list_init_err:

	return rc;
}

static int oplus_chg_vb_exit(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *chip;
	int i;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	chip = oplus_chg_ic_get_drvdata(ic_dev);
	devm_free_irq(chip->dev, chip->ccdetect_irq, chip);
	for (i = 0; i < chip->child_num; i++) {
		rc = oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_EXIT);
		if (rc < 0)
			chg_err("child ic[%d] exit error, rc=%d\n", i, rc);
	}
	for (i = 0; i < chip->child_num; i++) {
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_ERR)) {
			oplus_chg_ic_virq_release(chip->child_list[i].ic_dev,
						  OPLUS_IC_VIRQ_ERR, chip);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_CC_DETECT)) {
			oplus_chg_ic_virq_release(chip->child_list[i].ic_dev,
						  OPLUS_IC_VIRQ_CC_DETECT, chip);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_PLUGIN)) {
			oplus_chg_ic_virq_release(chip->child_list[i].ic_dev,
						  OPLUS_IC_VIRQ_PLUGIN, chip);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_CC_CHANGED)) {
			oplus_chg_ic_virq_release(chip->child_list[i].ic_dev,
						  OPLUS_IC_VIRQ_CC_CHANGED, chip);
		}
	}
	for (i = 0; i < chip->child_num; i++) {
		if (chip->child_list[i].virqs != NULL)
			devm_kfree(chip->dev, chip->child_list[i].virqs);
	}
	for (i = 0; i < chip->child_num; i++)
		devm_kfree(chip->dev, chip->child_list[i].funcs);

	return 0;
}

static int oplus_chg_vb_reg_dump(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_REG_DUMP);
		if (rc < 0)
			chg_err("child ic[%d] exit error, rc=%d\n", i, rc);
	}

	return 0;
}

static int oplus_chg_vb_smt_test(struct oplus_chg_ic_dev *ic_dev, char buf[], int len)
{
	struct oplus_virtual_buck_ic *vb;
	int i, index;
	int rc;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	index = 0;
	for (i = 0; i < vb->child_num; i++) {
		if (index >= len)
			return len;
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_SMT_TEST, buf + index,
				       len - index);
		if (rc < 0) {
			if (rc != -ENOTSUPP) {
				chg_err("child ic[%d] smt test error, rc=%d\n",
					i, rc);
				rc = snprintf(buf + index, len - index,
					"[%s]-[%s]:%d\n",
					vb->child_list[i].ic_dev->manu_name,
					"FUNC_ERR", rc);
			} else {
				rc = 0;
			}
		} else {
			if ((rc > 0) && buf[index + rc - 1] != '\n') {
				buf[index + rc] = '\n';
				index++;
			}
		}
		index += rc;
	}

	return index;
}

static int oplus_chg_vb_input_present(struct oplus_chg_ic_dev *ic_dev, bool *present)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_PRESENT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*present = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_PRESENT)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_PRESENT, present);
			if (rc < 0) {
				chg_err("child ic[%d] get input present status error, rc=%d\n", i, rc);
				continue;
			}
			break;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_PRESENT)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_PRESENT, present);
			if (rc < 0)
				chg_err("child ic[%d] get input present status error, rc=%d\n", i, rc);
			break;
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_input_suspend(struct oplus_chg_ic_dev *ic_dev, bool suspend)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
	bool suspend_temp;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		suspend = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND))
				continue;
			if (vb->child_list[i].current_ratio == 0)
				suspend_temp = true;
			else
				suspend_temp = suspend;
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, suspend_temp);
			if (rc < 0) {
				chg_err("child ic[%d] input %s error, rc=%d\n", i, suspend_temp ? "suspend" : "unsuspend", rc);
				return rc;
			}
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, suspend);
			if (rc < 0) {
				chg_err("child ic[%d] input %s error, rc=%d\n", i, suspend ? "suspend" : "unsuspend", rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int oplus_chg_vb_input_is_suspend(struct oplus_chg_ic_dev *ic_dev, bool *suspend)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
	bool suspend_temp = true;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*suspend = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	*suspend = true;

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND))
				continue;
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, &suspend_temp);
			if (rc < 0) {
				chg_err("child ic[%d] get input suspend status error, rc=%d\n", i, rc);
				return rc;
			}
			*suspend &= suspend_temp;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, suspend);
			if (rc < 0) {
				chg_err("child ic[%d] input %s error, rc=%d\n", i, suspend ? "suspend" : "unsuspend", rc);
				return rc;
			}
			if (suspend)
				return 0;
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int oplus_chg_vb_output_suspend(struct oplus_chg_ic_dev *ic_dev, bool suspend)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
	bool suspend_temp;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		suspend = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND))
				continue;
			if (vb->child_list[i].current_ratio == 0)
				suspend_temp = true;
			else
				suspend_temp = suspend;
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, suspend_temp);
			if (rc < 0) {
				chg_err("child ic[%d] output %s error, rc=%d\n", i, suspend_temp ? "suspend" : "unsuspend", rc);
				return rc;
			}
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, suspend);
			if (rc < 0) {
				chg_err("child ic[%d] output %s error, rc=%d\n", i, suspend ? "suspend" : "unsuspend", rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int oplus_chg_vb_output_is_suspend(struct oplus_chg_ic_dev *ic_dev, bool *suspend)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
	bool suspend_temp = true;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*suspend = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	*suspend = true;

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND))
				continue;
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, &suspend_temp);
			if (rc < 0) {
				chg_err("child ic[%d] get output suspend status error, rc=%d\n", i, rc);
				return rc;
			}
			*suspend &= suspend_temp;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, suspend);
			if (rc < 0) {
				chg_err("child ic[%d] output %s error, rc=%d\n", i, suspend ? "suspend" : "unsuspend", rc);
				return rc;
			}
			if (suspend)
				return 0;
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int oplus_chg_vb_set_icl(struct oplus_chg_ic_dev *ic_dev, int icl_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_ICL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		icl_ma = oplus_chg_ic_get_item_data(buf, 0);
		chg_err("wkcs: overwrite icl_ma = %d\n", icl_ma);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_ICL))
				continue;
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_SET_ICL,
				icl_ma * vb->child_list[i].current_ratio / 100);
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_ICL)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			return -EINVAL; /* TODO */
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (rc < 0) {
			chg_err("child ic[%d] set icl error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return 0;
}

static int oplus_chg_vb_get_icl(struct oplus_chg_ic_dev *ic_dev, int *icl_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int temp_icl_ma;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_ICL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*icl_ma = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	*icl_ma = 0;
	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i],
				     OPLUS_IC_FUNC_BUCK_GET_ICL))
			continue;
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i],
					     OPLUS_IC_FUNC_BUCK_GET_ICL))
				continue;
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_ICL,
				&temp_icl_ma);
			if (rc < 0) {
				chg_err("child ic[%d] get icl error, rc=%d\n", i, rc);
				return rc;
			}
			*icl_ma += temp_icl_ma;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_ICL)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			if (i == 0) {
				rc = oplus_chg_ic_func(
					vb->child_list[i].ic_dev,
					OPLUS_IC_FUNC_BUCK_GET_ICL, icl_ma);
				if (rc < 0) {
					chg_err("child ic[%d] get icl error, rc=%d\n", i, rc);
					return rc;
				}
				continue;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int oplus_chg_vb_set_fcc(struct oplus_chg_ic_dev *ic_dev, int fcc_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_FCC);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		fcc_ma = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_FCC))
				continue;
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_SET_FCC,
				fcc_ma * vb->child_list[i].current_ratio / 100);
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_FCC)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			return -EINVAL; /* TODO */
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (rc < 0) {
			chg_err("child ic[%d] set fcc error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return 0;
}

static int oplus_chg_vb_set_fv(struct oplus_chg_ic_dev *ic_dev, int fv_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_FV);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		fv_mv = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (vb->child_list[i].current_ratio == 0)
				continue;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i < vb->child_num - 1)
				continue;
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (!func_is_support(&vb->child_list[i],
				     OPLUS_IC_FUNC_BUCK_SET_FV))
			return -EINVAL;
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_BUCK_SET_FV, fv_mv);
		if (rc < 0) {
			chg_err("child ic[%d] set fcc error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return 0;
}

static int oplus_chg_vb_set_iterm(struct oplus_chg_ic_dev *ic_dev, int iterm_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_ITERM);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		iterm_ma = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (vb->child_list[i].current_ratio == 0)
				continue;
			if (i != vb->main_charger)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_ITERM)) {
				chg_err("main charger must support this function\n");
				return -EINVAL;
			}
			/*
			 * TODO:
			 * When the charging current is less than a certain
			 * threshold, all auxiliary charger should be turned
			 * off, and only the cut-off current should be set for
			 * the main charger.
			 */
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_SET_ITERM,
				iterm_ma);
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			return -EINVAL; /* TODO */
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (rc < 0) {
			chg_err("child ic[%d] set fcc error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return 0;
}

static int oplus_chg_vb_set_rechg_vol(struct oplus_chg_ic_dev *ic_dev, int vol_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		vol_mv = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (vb->child_list[i].current_ratio == 0)
				continue;
			if (i != vb->main_charger)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL)) {
				chg_err("main charger must support this function\n");
				return -EINVAL;
			}
			/*
			 * TODO:
			 * Pre-charging is only allowed by the main charger.
			 */
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL,
				vol_mv);
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			return -EINVAL; /* TODO */
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (rc < 0) {
			chg_err("child ic[%d] set fcc error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return 0;
}

static int oplus_chg_vb_get_input_curr(struct oplus_chg_ic_dev *ic_dev, int *curr_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int curr_temp;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*curr_ma = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	*curr_ma = 0;
	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i],
				     OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR))
			continue;
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i],
					     OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR))
				continue;
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR,
				&curr_temp);
			if (rc < 0) {
				chg_err("child ic[%d] get intput current error, rc=%d\n", i, rc);
				return rc;
			}
			*curr_ma += curr_temp;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR)) {
				chg_err("for serial connection, all ICs must support this function\n");
				return -EINVAL;
			}
			if (i == 0) {
				rc = oplus_chg_ic_func(
					vb->child_list[i].ic_dev,
					OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR, curr_ma);
				if (rc < 0) {
					chg_err("child ic[%d] get intput current error, rc=%d\n", i, rc);
					return rc;
				}
				continue;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_get_input_vol(struct oplus_chg_ic_dev *ic_dev, int *vol_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*vol_mv = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	*vol_mv = 0;
	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL)) {
				rc = -ENOTSUPP;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL,
				vol_mv);
			if (rc < 0)
				chg_err("child ic[%d] get input voltage error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -ENOTSUPP;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL,
				vol_mv);
			if (rc < 0) {
				chg_err("child ic[%d] input voltage, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_otg_boost_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_OTG_BOOST_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_OTG_BOOST_ENABLE)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_OTG_BOOST_ENABLE,
				en);
			if (rc < 0)
				chg_err("child ic[%d] set otg boost %s error, rc=%d\n", i, en ? "enable" : "disable", rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_OTG_BOOST_ENABLE)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_OTG_BOOST_ENABLE,
				en);
			if (rc < 0) {
				chg_err("child ic[%d] set otg boost %s error, rc=%d\n", i, en ? "enable" : "disable", rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_otg_boost_vol(struct oplus_chg_ic_dev *ic_dev, int vol_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_OTG_BOOST_VOL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		vol_mv = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_OTG_BOOST_VOL)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_OTG_BOOST_VOL,
				vol_mv);
			if (rc < 0)
				chg_err("child ic[%d] set otg boost vol error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_OTG_BOOST_VOL)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_OTG_BOOST_VOL,
				vol_mv);
			if (rc < 0) {
				chg_err("child ic[%d] set otg boost vol error, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_otg_boost_curr_limit(struct oplus_chg_ic_dev *ic_dev, int curr_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		curr_ma = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT,
				curr_ma);
			if (rc < 0)
				chg_err("child ic[%d] set otg boost curr limit error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT,
				curr_ma);
			if (rc < 0) {
				chg_err("child ic[%d] set otg boost error, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_aicl_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_AICL_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	/* TODO */

	return rc;
}

static int oplus_chg_vb_aicl_rerun(struct oplus_chg_ic_dev *ic_dev)
{
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO */

	return rc;
}

static int oplus_chg_vb_aicl_reset(struct oplus_chg_ic_dev *ic_dev)
{
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	/* TODO */

	return rc;
}

static int oplus_chg_vb_get_cc_orientation(struct oplus_chg_ic_dev *ic_dev, int *orientation)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*orientation = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION,
			orientation);
		if (rc < 0)
			chg_err("child ic[%d] get cc orientation error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_get_hw_detect(struct oplus_chg_ic_dev *ic_dev, int *detected)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_HW_DETECT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*detected = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (oplus_vc_ccdetect_gpio_support(vb)) {
		*detected = !gpio_get_value(vb->misc_gpio.ccdetect_gpio);
		chg_info("hw_detect=%d\n", *detected);
		return 0;
	}

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_HW_DETECT)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_GET_HW_DETECT,
			detected);
		if (rc < 0)
			chg_err("child ic[%d] get hw detect error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_get_charger_type(struct oplus_chg_ic_dev *ic_dev, int *type)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*type = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE,
			type);
		if (rc < 0)
			chg_err("child ic[%d] get charger type error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_rerun_bc12(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_RERUN_BC12)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_RERUN_BC12);
		if (rc < 0)
			chg_err("child ic[%d] rerun bc1.2 error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_qc_detect_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE,
			en);
		if (rc < 0)
			chg_err("child ic[%d] %s qc detect error, rc=%d\n", i, en ? "enable" : "disable", rc);
		else
			return 0;
	}

	return rc;
}

#define PWM_COUNT	5
static int oplus_chg_vb_shipmod_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (gpio_is_valid(vb->misc_gpio.ship_gpio)) {
		chg_info("select gpio ship mode control\n");
		pinctrl_select_state(vb->misc_gpio.pinctrl, vb->misc_gpio.ship_sleep);
		for (i = 0; i < PWM_COUNT; i++) {
			pinctrl_select_state(vb->misc_gpio.pinctrl, vb->misc_gpio.ship_active);
			mdelay(3);
			pinctrl_select_state(vb->misc_gpio.pinctrl, vb->misc_gpio.ship_sleep);
			mdelay(3);
		}
		chg_info("power off after 15s\n");
		return 0;
	}

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE,
			en);
		if (rc < 0)
			chg_err("child ic[%d] %s shipmod error, rc=%d\n", i, en ? "enable" : "disable", rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_set_qc_config(struct oplus_chg_ic_dev *ic_dev, enum oplus_chg_qc_version version, int vol_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 2))
			return -EINVAL;
		version = oplus_chg_ic_get_item_data(buf, 0);
		vol_mv = oplus_chg_ic_get_item_data(buf, 1);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG,
			version, vol_mv);
		if (rc < 0)
			chg_err("child ic[%d] set qc to %dmV error, rc=%d\n", i, vol_mv, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_set_pd_config(struct oplus_chg_ic_dev *ic_dev, u32 pdo)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		pdo = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(
			vb->child_list[i].ic_dev,
			OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG,
			pdo);
		if (rc < 0)
			chg_err("child ic[%d] set pdo(=0x%08x) error, rc=%d\n", i, pdo, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_wls_boost_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_WLS_BOOST_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_WLS_BOOST_ENABLE)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_WLS_BOOST_ENABLE,
				en);
			if (rc < 0)
				chg_err("child ic[%d] set wls boost %s error, rc=%d\n", i, en ? "enable" : "disable", rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_WLS_BOOST_ENABLE)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_WLS_BOOST_ENABLE,
				en);
			if (rc < 0) {
				chg_err("child ic[%d] set wls boost %s error, rc=%d\n", i, en ? "enable" : "disable", rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_wls_boost_vol(struct oplus_chg_ic_dev *ic_dev, int vol_mv)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_WLS_BOOST_VOL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		vol_mv = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_WLS_BOOST_VOL)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_WLS_BOOST_VOL,
				vol_mv);
			if (rc < 0)
				chg_err("child ic[%d] set wls boost vol error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_WLS_BOOST_VOL)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_WLS_BOOST_VOL,
				vol_mv);
			if (rc < 0) {
				chg_err("child ic[%d] set wls boost vol error, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_wls_boost_curr_limit(struct oplus_chg_ic_dev *ic_dev, int curr_ma)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		curr_ma = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT,
				curr_ma);
			if (rc < 0)
				chg_err("child ic[%d] set wls boost curr limit error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT,
				curr_ma);
			if (rc < 0) {
				chg_err("child ic[%d] set wls boost error, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

static int oplus_chg_vb_gauge_update(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_GAUGE_UPDATE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GAUGE_UPDATE);
		if (rc < 0)
			chg_err("child ic[%d] gauge update error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_voocphy_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_VOOCPHY_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_VOOCPHY_ENABLE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_VOOCPHY_ENABLE, en);
		if (rc < 0)
			chg_err("child ic[%d] voocphy %s error, rc=%d\n", i, en ? "enable" : "disable", rc);

		return rc;
	}

	return rc;
}

static int oplus_chg_vb_voocphy_reset_again(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_VOOCPHY_RESET_AGAIN)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_VOOCPHY_RESET_AGAIN);
		if (rc < 0)
			chg_err("child ic[%d] voocphy reset again error, rc=%d\n", i, rc);

		return rc;
	}

	return rc;
}

static int oplus_chg_vb_get_charger_cycle(struct oplus_chg_ic_dev *ic_dev, int *cycle)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_CHARGER_CYCLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*cycle = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_GET_CHARGER_CYCLE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GET_CHARGER_CYCLE, cycle);
		if (rc < 0)
			chg_err("child ic[%d] get charger cycle error, rc=%d\n", i, rc);
		else
			return 0;
	}

	return rc;
}

static int oplus_chg_vb_get_shutdown_soc(struct oplus_chg_ic_dev *ic_dev, int *soc)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_SHUTDOWN_SOC);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*soc = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_GET_SHUTDOWN_SOC)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GET_SHUTDOWN_SOC, soc);
		if (rc < 0)
			chg_err("child ic[%d] get shutdown soc error, rc=%d\n", i, rc);

		return rc;
	}

	return rc;
}

static int oplus_chg_vb_backup_soc(struct oplus_chg_ic_dev *ic_dev, int soc)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BACKUP_SOC);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		soc = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BACKUP_SOC)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_BACKUP_SOC, soc);
		if (rc < 0)
			chg_err("child ic[%d] backup soc error, rc=%d\n", i, rc);

		return rc;
	}

	return rc;
}

static int oplus_chg_vb_get_vbus_collapse_status(struct oplus_chg_ic_dev *ic_dev, bool *collapse)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*collapse = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS)) {
				rc = -EINVAL;
				continue;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS,
				collapse);
			if (rc < 0)
				chg_err("child ic[%d] get vbus collapse status error, rc=%d\n", i, rc);
			else
				return 0;
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue;
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS)) {
				chg_err("for serial connection, first IC must support this function\n");
				return -EINVAL;
			}
			rc = oplus_chg_ic_func(
				vb->child_list[i].ic_dev,
				OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS,
				collapse);
			if (rc < 0) {
				chg_err("child ic[%d] get vbus collapse status error, rc=%d\n", i, rc);
				return rc;
			}
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
	}

	return rc;
}

#define USBTEMP_DEFAULT_VOLT_VALUE_MV 950
static int oplus_chg_vb_get_usb_temp_volt(struct oplus_chg_ic_dev *ic_dev, int *vol_l, int *vol_r)
{
	struct oplus_virtual_buck_ic *vb;
	static int usbtemp_volt_l_pre = USBTEMP_DEFAULT_VOLT_VALUE_MV;
	static int usbtemp_volt_r_pre = USBTEMP_DEFAULT_VOLT_VALUE_MV;
	int usbtemp_volt = 0;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		if (vol_l != NULL)
			*vol_l = usbtemp_volt_l_pre;
		if (vol_r != NULL)
			*vol_r = usbtemp_volt_r_pre;
		return 0;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_USB_TEMP_VOLT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 2))
			return -EINVAL;
		*vol_l = oplus_chg_ic_get_item_data(buf, 0);
		*vol_r = oplus_chg_ic_get_item_data(buf, 1);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (vol_l == NULL)
		goto usbtemp_next;

	if (IS_ERR_OR_NULL(vb->usbtemp_v_chan)) {
		chg_err("usbtemp_v_chan is NULL!\n");
		*vol_l = usbtemp_volt_l_pre;
		goto usbtemp_next;
	}

	rc = iio_read_channel_processed(vb->usbtemp_v_chan, &usbtemp_volt);
	if (rc < 0) {
		chg_err("usbtemp_volt_l read error\n");
		*vol_l = usbtemp_volt_l_pre;
		goto usbtemp_next;
	}

	usbtemp_volt = 18 * usbtemp_volt / 10000;
	if (usbtemp_volt > USBTEMP_DEFAULT_VOLT_VALUE_MV) {
		usbtemp_volt = USBTEMP_DEFAULT_VOLT_VALUE_MV;
	}

	*vol_l = usbtemp_volt;
	usbtemp_volt_l_pre = usbtemp_volt;

usbtemp_next:
	if (vol_r == NULL)
		return 0;

	usbtemp_volt = 0;
	if (IS_ERR_OR_NULL(vb->usbtemp_sup_v_chan)) {
		chg_err("usbtemp_sup_v_chan is NULL!\n");
		*vol_r = usbtemp_volt_r_pre;
		return 0;
	}

	rc = iio_read_channel_processed(vb->usbtemp_sup_v_chan, &usbtemp_volt);
	if (rc < 0) {
		chg_err("usbtemp_volt_r read error\n");
		*vol_r = usbtemp_volt_r_pre;
		return 0;
	}

	usbtemp_volt = 18 * usbtemp_volt / 10000;
	if (usbtemp_volt > USBTEMP_DEFAULT_VOLT_VALUE_MV) {
		usbtemp_volt = USBTEMP_DEFAULT_VOLT_VALUE_MV;
	}

	*vol_r = usbtemp_volt;
	usbtemp_volt_r_pre = usbtemp_volt;

	return 0;
}

static int oplus_chg_vb_get_usb_temp(struct oplus_chg_ic_dev *ic_dev, int *temp_l, int *temp_r)
{
	struct oplus_virtual_buck_ic *vb;
	int vol_l, vol_r;
	int i;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_USB_TEMP);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 2))
			return -EINVAL;
		*temp_l = oplus_chg_ic_get_item_data(buf, 0);
		*temp_r = oplus_chg_ic_get_item_data(buf, 1);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (vb->adc_vol_temp_info == NULL)
		vb->adc_vol_temp_info = &adc_vol_temp_info_table[0];

	(void)oplus_chg_vb_get_usb_temp_volt(ic_dev, &vol_l, &vol_r);

	if (temp_l == NULL)
		goto next;

	for (i = vb->adc_vol_temp_info->con_volt_table_size - 1; i >= 0; i--) {
		if (vb->adc_vol_temp_info->con_volt_table[i] >= vol_l)
			break;
		else if (i == 0)
			break;
	}

	*temp_l = vb->adc_vol_temp_info->con_temp_table[i];

next:
	if (temp_r == NULL)
		return 0;

	for (i = vb->adc_vol_temp_info->con_volt_table_size - 1; i >= 0; i--) {
		if (vb->adc_vol_temp_info->con_volt_table[i] >= vol_r)
			break;
		else if (i == 0)
			break;
	}

	*temp_r = vb->adc_vol_temp_info->con_temp_table[i];

	return 0;
}

static int oplus_chg_vb_usb_temp_check_is_support(struct oplus_chg_ic_dev *ic_dev, bool *support)
{
	struct oplus_virtual_buck_ic *vb;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_USB_TEMP_CHECK_IS_SUPPORT);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*support = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	*support = oplus_vc_usbtemp_check_is_support(vb);

	return 0;
}

static int oplus_chg_vb_get_typec_mode(struct oplus_chg_ic_dev *ic_dev,
				       enum oplus_chg_typec_port_role_type *mode)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_TYPEC_MODE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*mode = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_GET_TYPEC_MODE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GET_TYPEC_MODE,
				       mode);
		if (rc < 0)
			chg_err("child ic[%d] get typec mode error, rc=%d\n", i, rc);
		return rc;
	}

	return rc;
}

static int oplus_chg_vb_set_typec_mode(struct oplus_chg_ic_dev *ic_dev,
				       enum oplus_chg_typec_port_role_type mode)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_TYPEC_MODE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		mode = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_TYPEC_MODE)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_SET_TYPEC_MODE,
				       mode);
		if (rc < 0)
			chg_err("child ic[%d] set typec mode(=%d) error, rc=%d\n", i, mode, rc);
		return rc;
	}

	return rc;
}

static int oplus_chg_vb_set_usb_dischg_enable(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_USB_DISCHG_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (IS_ERR_OR_NULL(vb->misc_gpio.pinctrl) ||
	    IS_ERR_OR_NULL(vb->misc_gpio.dischg_disable) ||
	    IS_ERR_OR_NULL(vb->misc_gpio.dischg_enable)) {
		chg_err("pinctrl 、 dischg_disable or dischg_enable is NULL\n");
		return -EINVAL;
	}

	rc = pinctrl_select_state(vb->misc_gpio.pinctrl,
				  en ? vb->misc_gpio.dischg_enable :
					     vb->misc_gpio.dischg_disable);
	if (rc < 0)
		chg_err("can't %s usb dischg, rc=%d\n", en ? "enable" : "disable", rc);

	return rc;
}

static int oplus_chg_vb_get_usb_dischg_status(struct oplus_chg_ic_dev *ic_dev, bool *en)
{
	struct oplus_virtual_buck_ic *vb;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_USB_DISCHG_STATUS);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*en = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (!oplus_vc_usbtemp_check_is_support(vb)) {
		*en = false;
		return 0;
	}
	*en = !!gpio_get_value(vb->misc_gpio.dischg_gpio);

	return 0;
}

static int oplus_chg_vb_set_otg_switch_status(struct oplus_chg_ic_dev *ic_dev, bool en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int level = 0;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		en = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (oplus_vc_ccdetect_gpio_support(vb)) {
		level = gpio_get_value(vb->misc_gpio.ccdetect_gpio);
		if (level != 1) {
			chg_err("gpio[%s], should set, return", level ? "H" : "L");
			return 0;
		}

		rc = oplus_ccdetect_enable(vb, en);
		if (rc < 0)
			chg_err("%s ccdetect error, rc=%d\n", en ? "enable" : "disable", rc);
		else
			vb->otg_switch = en;

		return rc;
	}

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS,
				       en);
		if (rc < 0)
			chg_err("child ic[%d] %s otg switch error, rc=%d\n", i, en ? "enable" : "disable", rc);
		else
			vb->otg_switch = en;

		return rc;
	}

	return rc;
}

static int oplus_chg_vb_get_otg_switch_status(struct oplus_chg_ic_dev *ic_dev, bool *en)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*en = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (oplus_vc_ccdetect_gpio_support(vb)) {
		*en = vb->otg_switch;
		return 0;
	}

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS)) {
			rc = -EINVAL;
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS,
				       en);
		if (rc < 0)
			chg_err("child ic[%d] get otg switch status error, rc=%d\n", i, rc);
		return rc;
	}

	return rc;
}

#define DISCONNECT			0
#define STANDARD_TYPEC_DEV_CONNECT	BIT(0)
#define OTG_DEV_CONNECT			BIT(1)
static int oplus_chg_vb_get_otg_online_status(struct oplus_chg_ic_dev *ic_dev, int *status)
{
	struct oplus_virtual_buck_ic *vb;
	int online = 0;
	enum oplus_chg_typec_port_role_type typec_mode;
	int hw_detect = 0;
	int level;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_GET_OTG_ONLINE_STATUS);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		*status = oplus_chg_ic_get_item_data(buf, 0);
		return 0;
	}
#endif

	vb = oplus_chg_ic_get_drvdata(ic_dev);
	if (oplus_vc_ccdetect_gpio_support(vb)) {
		level = gpio_get_value(vb->misc_gpio.ccdetect_gpio);
		if (level != gpio_get_value(vb->misc_gpio.ccdetect_gpio)) {
			chg_info("ccdetect_gpio is unstable, try again...\n");
			usleep_range(5000, 5100);
			level = gpio_get_value(vb->misc_gpio.ccdetect_gpio);
		}
		online = (level == 1) ? DISCONNECT : STANDARD_TYPEC_DEV_CONNECT;
	} else {
		/* The error returned may be that this function is not supported */
		rc = oplus_chg_vb_get_hw_detect(ic_dev, &hw_detect);
		if (rc < 0) {
			chg_err("get hw detect status error, rc=%d\n", rc);
			return rc;
		} else {
			online = (hw_detect == 1) ? STANDARD_TYPEC_DEV_CONNECT : DISCONNECT;
		}
	}

	rc = oplus_chg_vb_get_typec_mode(ic_dev, &typec_mode);
	if (rc < 0) {
		chg_err("get typec mode error, rc=%d\n", rc);
		return rc;
	}

	online = online | ((typec_mode == TYPEC_PORT_ROLE_SNK) ? OTG_DEV_CONNECT : DISCONNECT);
	*status = online;

	return rc;
}

static int oplus_chg_vb_cc_detect_happened(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	vb = oplus_chg_ic_get_drvdata(ic_dev);

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_CC_DETECT_HAPPENED)) {
			continue;
		}
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_CC_DETECT_HAPPENED);
		if (rc < 0)
			chg_err("child ic[%d] send cc detect happend error, rc=%d\n", i, rc);
	}

	return rc;
}

static int oplus_chg_vb_curr_drop(struct oplus_chg_ic_dev *ic_dev)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;

	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	vb = oplus_chg_ic_get_drvdata(ic_dev);

	for (i = 0; i < vb->child_num; i++) {
		if (vb->connect_type == OPLUS_CHG_IC_CONNECT_PARALLEL) {
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_CURR_DROP))
				continue;
			if (vb->child_list[i].current_ratio == 0)
				continue;
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
					       OPLUS_IC_FUNC_BUCK_CURR_DROP);
		} else if (vb->connect_type == OPLUS_CHG_IC_CONNECT_SERIAL) {
			if (i != 0)
				continue; /* only need to process the first IC */
			if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_CURR_DROP)) {
				return 0;
			}
			rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
					       OPLUS_IC_FUNC_BUCK_CURR_DROP);
		} else {
			chg_err("Unknown connect type\n");
			return -EINVAL;
		}
		if (rc < 0) {
			chg_err("child ic[%d] set curr drop error, rc=%d\n", i, rc);
			return rc;
		}
	}

	return rc;
}

static int oplus_chg_vb_wdt_enable(struct oplus_chg_ic_dev *ic_dev, bool enable)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif


	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	vb = oplus_chg_ic_get_drvdata(ic_dev);

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_BUCK_WDT_ENABLE);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		enable = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_BUCK_WDT_ENABLE))
			continue;
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_BUCK_WDT_ENABLE,
				       enable);
		if (rc < 0) {
			chg_err("child ic[%d] %s wdt error, rc=%d\n", i, enable ? "enable" : "disable", rc);
			return rc;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_curr_level(struct oplus_chg_ic_dev *ic_dev, int cool_down)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif


	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	vb = oplus_chg_ic_get_drvdata(ic_dev);

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		cool_down = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL))
			continue;
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL,
				       cool_down);
		if (rc < 0) {
			chg_err("child ic[%d] set current level to %d error, rc=%d\n", i, cool_down, rc);
			return rc;
		}
	}

	return rc;
}

static int oplus_chg_vb_set_match_temp(struct oplus_chg_ic_dev *ic_dev, int match_temp)
{
	struct oplus_virtual_buck_ic *vb;
	int i;
	int rc = 0;
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	struct oplus_chg_ic_overwrite_data *data;
	const void *buf;
#endif


	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	vb = oplus_chg_ic_get_drvdata(ic_dev);

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	data = oplus_chg_ic_get_overwrite_data(ic_dev, OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP);
	if (unlikely(data != NULL)) {
		buf = (const void *)data->buf;
		if (!oplus_chg_ic_debug_data_check(buf, data->size, 1))
			return -EINVAL;
		match_temp = oplus_chg_ic_get_item_data(buf, 0);
	}
#endif

	for (i = 0; i < vb->child_num; i++) {
		if (!func_is_support(&vb->child_list[i], OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP))
			continue;
		rc = oplus_chg_ic_func(vb->child_list[i].ic_dev,
				       OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP,
				       match_temp);
		if (rc < 0) {
			chg_err("child ic[%d] set match temp to %d error, rc=%d\n", i, match_temp, rc);
			return rc;
		}
	}

	return rc;
}

static void *oplus_chg_vb_get_func(struct oplus_chg_ic_dev *ic_dev, enum oplus_chg_ic_func func_id)
{
	void *func = NULL;

	if (!ic_dev->online && (func_id != OPLUS_IC_FUNC_INIT) &&
	    (func_id != OPLUS_IC_FUNC_EXIT))
		return NULL;

	switch (func_id) {
	case OPLUS_IC_FUNC_INIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_INIT, oplus_chg_vb_init);
		break;
	case OPLUS_IC_FUNC_EXIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_EXIT, oplus_chg_vb_exit);
		break;
	case OPLUS_IC_FUNC_REG_DUMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_REG_DUMP, oplus_chg_vb_reg_dump);
		break;
	case OPLUS_IC_FUNC_SMT_TEST:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SMT_TEST, oplus_chg_vb_smt_test);
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_PRESENT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_INPUT_PRESENT, oplus_chg_vb_input_present);
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, oplus_chg_vb_input_suspend);
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND, oplus_chg_vb_input_is_suspend);
		break;
	case OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND, oplus_chg_vb_output_suspend);
		break;
	case OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND, oplus_chg_vb_output_is_suspend);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_ICL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_ICL, oplus_chg_vb_set_icl);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_ICL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_ICL, oplus_chg_vb_get_icl);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_FCC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_FCC, oplus_chg_vb_set_fcc);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_FV:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_FV, oplus_chg_vb_set_fv);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_ITERM:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_ITERM, oplus_chg_vb_set_iterm);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL, oplus_chg_vb_set_rechg_vol);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR, oplus_chg_vb_get_input_curr);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL, oplus_chg_vb_get_input_vol);
		break;
	case OPLUS_IC_FUNC_OTG_BOOST_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_OTG_BOOST_ENABLE, oplus_chg_vb_otg_boost_enable);
		break;
	case OPLUS_IC_FUNC_SET_OTG_BOOST_VOL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_OTG_BOOST_VOL, oplus_chg_vb_set_otg_boost_vol);
		break;
	case OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT, oplus_chg_vb_set_otg_boost_curr_limit);
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_AICL_ENABLE, oplus_chg_vb_aicl_enable);
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_RERUN:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_AICL_RERUN, oplus_chg_vb_aicl_rerun);
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_RESET:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_AICL_RESET, oplus_chg_vb_aicl_reset);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION, oplus_chg_vb_get_cc_orientation);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_HW_DETECT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_HW_DETECT, oplus_chg_vb_get_hw_detect);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE, oplus_chg_vb_get_charger_type);
		break;
	case OPLUS_IC_FUNC_BUCK_RERUN_BC12:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_RERUN_BC12, oplus_chg_vb_rerun_bc12);
		break;
	case OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE, oplus_chg_vb_qc_detect_enable);
		break;
	case OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE, oplus_chg_vb_shipmod_enable);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG, oplus_chg_vb_set_qc_config);
		break;
	case OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG, oplus_chg_vb_set_pd_config);
		break;
	case OPLUS_IC_FUNC_WLS_BOOST_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_WLS_BOOST_ENABLE, oplus_chg_vb_wls_boost_enable);
		break;
	case OPLUS_IC_FUNC_SET_WLS_BOOST_VOL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_WLS_BOOST_VOL, oplus_chg_vb_set_wls_boost_vol);
		break;
	case OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT, oplus_chg_vb_set_wls_boost_curr_limit);
		break;
	case OPLUS_IC_FUNC_GAUGE_UPDATE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GAUGE_UPDATE, oplus_chg_vb_gauge_update);
		break;
	case OPLUS_IC_FUNC_VOOCPHY_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_VOOCPHY_ENABLE, oplus_chg_vb_voocphy_enable);
		break;
	case OPLUS_IC_FUNC_VOOCPHY_RESET_AGAIN:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_VOOCPHY_RESET_AGAIN, oplus_chg_vb_voocphy_reset_again);
		break;
	case OPLUS_IC_FUNC_GET_CHARGER_CYCLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_CHARGER_CYCLE, oplus_chg_vb_get_charger_cycle);
		break;
	case OPLUS_IC_FUNC_GET_SHUTDOWN_SOC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_SHUTDOWN_SOC, oplus_chg_vb_get_shutdown_soc);
		break;
	case OPLUS_IC_FUNC_BACKUP_SOC:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BACKUP_SOC, oplus_chg_vb_backup_soc);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS, oplus_chg_vb_get_vbus_collapse_status);
		break;
	case OPLUS_IC_FUNC_GET_USB_TEMP_VOLT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_USB_TEMP_VOLT, oplus_chg_vb_get_usb_temp_volt);
		break;
	case OPLUS_IC_FUNC_GET_USB_TEMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_USB_TEMP, oplus_chg_vb_get_usb_temp);
		break;
	case OPLUS_IC_FUNC_USB_TEMP_CHECK_IS_SUPPORT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_USB_TEMP_CHECK_IS_SUPPORT, oplus_chg_vb_usb_temp_check_is_support);
		break;
	case OPLUS_IC_FUNC_GET_TYPEC_MODE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_TYPEC_MODE, oplus_chg_vb_get_typec_mode);
		break;
	case OPLUS_IC_FUNC_SET_TYPEC_MODE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_TYPEC_MODE, oplus_chg_vb_set_typec_mode);
		break;
	case OPLUS_IC_FUNC_SET_USB_DISCHG_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_USB_DISCHG_ENABLE, oplus_chg_vb_set_usb_dischg_enable);
		break;
	case OPLUS_IC_FUNC_GET_USB_DISCHG_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_USB_DISCHG_STATUS, oplus_chg_vb_get_usb_dischg_status);
		break;
	case OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS, oplus_chg_vb_set_otg_switch_status);
		break;
	case OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS, oplus_chg_vb_get_otg_switch_status);
		break;
	case OPLUS_IC_FUNC_GET_OTG_ONLINE_STATUS:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_GET_OTG_ONLINE_STATUS, oplus_chg_vb_get_otg_online_status);
		break;
	case OPLUS_IC_FUNC_CC_DETECT_HAPPENED:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_CC_DETECT_HAPPENED, oplus_chg_vb_cc_detect_happened);
		break;
	case OPLUS_IC_FUNC_BUCK_CURR_DROP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_CURR_DROP, oplus_chg_vb_curr_drop);
		break;
	case OPLUS_IC_FUNC_BUCK_WDT_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_BUCK_WDT_ENABLE, oplus_chg_vb_wdt_enable);
		break;
	case OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL, oplus_chg_vb_set_curr_level);
		break;
	case OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP, oplus_chg_vb_set_match_temp);
		break;
	default:
		chg_err("this func(=%d) is not supported\n", func_id);
		func = NULL;
		break;
	}

	return func;
}

#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
static int oplus_chg_vb_set_func_data(struct oplus_chg_ic_dev *ic_dev,
				      enum oplus_chg_ic_func func_id,
				      const void *buf, size_t buf_len)
{
	int rc = 0;

	if (!ic_dev->online && (func_id != OPLUS_IC_FUNC_INIT) &&
	    (func_id != OPLUS_IC_FUNC_EXIT))
		return -EINVAL;

	switch (func_id) {
	case OPLUS_IC_FUNC_INIT:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_init(ic_dev);
		break;
	case OPLUS_IC_FUNC_EXIT:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_exit(ic_dev);
		break;
	case OPLUS_IC_FUNC_REG_DUMP:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_reg_dump(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_input_suspend(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_output_suspend(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_ICL:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_icl(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_FCC:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_fcc(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_FV:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_fv(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_ITERM:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_iterm(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_rechg_vol(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_OTG_BOOST_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_otg_boost_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_OTG_BOOST_VOL:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_otg_boost_vol(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_otg_boost_curr_limit(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_aicl_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_RERUN:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_aicl_rerun(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_AICL_RESET:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_aicl_reset(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_RERUN_BC12:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_rerun_bc12(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_qc_detect_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_shipmod_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 2))
			return -EINVAL;
		rc = oplus_chg_vb_set_qc_config(ic_dev, oplus_chg_ic_get_item_data(buf, 0), oplus_chg_ic_get_item_data(buf, 1));
		break;
	case OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_pd_config(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_WLS_BOOST_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_wls_boost_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_WLS_BOOST_VOL:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_wls_boost_vol(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_wls_boost_curr_limit(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_GAUGE_UPDATE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_gauge_update(ic_dev);
		break;
	case OPLUS_IC_FUNC_VOOCPHY_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_voocphy_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_VOOCPHY_RESET_AGAIN:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_voocphy_reset_again(ic_dev);
		break;
	case OPLUS_IC_FUNC_BACKUP_SOC:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_backup_soc(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_TYPEC_MODE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_typec_mode(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_USB_DISCHG_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_usb_dischg_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_otg_switch_status(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_CC_DETECT_HAPPENED:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_cc_detect_happened(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_CURR_DROP:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 0))
			return -EINVAL;
		rc = oplus_chg_vb_curr_drop(ic_dev);
		break;
	case OPLUS_IC_FUNC_BUCK_WDT_ENABLE:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_wdt_enable(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_VOOCPHY_SET_CURR_LEVEL:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_curr_level(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	case OPLUS_IC_FUNC_VOOCPHY_SET_MATCH_TEMP:
		if (!oplus_chg_ic_debug_data_check(buf, buf_len, 1))
			return -EINVAL;
		rc = oplus_chg_vb_set_match_temp(ic_dev, oplus_chg_ic_get_item_data(buf, 0));
		break;
	default:
		chg_err("this func(=%d) is not supported to set\n", func_id);
		return -ENOTSUPP;
		break;
	}

	return rc;
}

static ssize_t oplus_chg_vb_get_func_data(struct oplus_chg_ic_dev *ic_dev,
					  enum oplus_chg_ic_func func_id,
					  void *buf)
{
	bool temp;
	int *item_data;
	ssize_t rc = 0;
	int len;
	char *tmp_buf;

	if (!ic_dev->online && (func_id != OPLUS_IC_FUNC_INIT) &&
	    (func_id != OPLUS_IC_FUNC_EXIT))
		return -EINVAL;

	switch (func_id) {
	case OPLUS_IC_FUNC_SMT_TEST:
		tmp_buf = (char *)get_zeroed_page(GFP_KERNEL);
		if (!tmp_buf) {
			rc = -ENOMEM;
			break;
		}
		rc = oplus_chg_vb_smt_test(ic_dev, tmp_buf, PAGE_SIZE);
		if (rc < 0) {
			free_page((unsigned long)tmp_buf);
			break;
		}
		len = oplus_chg_ic_debug_str_data_init(buf, rc);
		memcpy(oplus_chg_ic_get_item_data_addr(buf, 0), tmp_buf, rc);
		free_page((unsigned long)tmp_buf);
		rc = len;
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_PRESENT:
		oplus_chg_ic_debug_data_init(buf, 1);
		rc = oplus_chg_vb_input_present(ic_dev, &temp);
		if (rc < 0)
			break;
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND:
		oplus_chg_ic_debug_data_init(buf, 1);
		rc = oplus_chg_vb_input_is_suspend(ic_dev, &temp);
		if (rc < 0)
			break;
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND:
		oplus_chg_ic_debug_data_init(buf, 1);
		rc = oplus_chg_vb_output_is_suspend(ic_dev, &temp);
		if (rc < 0)
			break;
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_ICL:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_icl(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_input_curr(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_input_vol(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_cc_orientation(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_HW_DETECT:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_hw_detect(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_charger_type(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_CHARGER_CYCLE:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_charger_cycle(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_SHUTDOWN_SOC:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_shutdown_soc(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_vbus_collapse_status(ic_dev, &temp);
		if (rc < 0)
			break;
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_USB_TEMP_VOLT:
		oplus_chg_ic_debug_data_init(buf, 2);
		rc = oplus_chg_vb_get_usb_temp_volt(
			ic_dev,
			oplus_chg_ic_get_item_data_addr(buf, 0),
			oplus_chg_ic_get_item_data_addr(buf, 1));
		if (rc < 0)
			break;
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		*item_data = cpu_to_le32(*item_data);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 1);
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(2);
		break;
	case OPLUS_IC_FUNC_GET_USB_TEMP:
		oplus_chg_ic_debug_data_init(buf, 2);
		rc = oplus_chg_vb_get_usb_temp(
			ic_dev,
			oplus_chg_ic_get_item_data_addr(buf, 0),
			oplus_chg_ic_get_item_data_addr(buf, 1));
		if (rc < 0)
			break;
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		*item_data = cpu_to_le32(*item_data);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 1);
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(2);
		break;
	case OPLUS_IC_FUNC_USB_TEMP_CHECK_IS_SUPPORT:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_usb_temp_check_is_support(ic_dev, &temp);
		if (rc < 0)
			break;
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_TYPEC_MODE:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_typec_mode(ic_dev, (enum oplus_chg_typec_port_role_type *)item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_USB_DISCHG_STATUS:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_usb_dischg_status(ic_dev, &temp);
		if (rc < 0)
			break;
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_otg_switch_status(ic_dev, &temp);
		if (rc < 0)
			break;
		*item_data = temp;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	case OPLUS_IC_FUNC_GET_OTG_ONLINE_STATUS:
		oplus_chg_ic_debug_data_init(buf, 1);
		item_data = oplus_chg_ic_get_item_data_addr(buf, 0);
		rc = oplus_chg_vb_get_otg_online_status(ic_dev, item_data);
		if (rc < 0)
			break;
		*item_data = cpu_to_le32(*item_data);
		rc = oplus_chg_ic_debug_data_size(1);
		break;
	default:
		chg_err("this func(=%d) is not supported to get\n", func_id);
		return -ENOTSUPP;
		break;
	}

	return rc;
}

enum oplus_chg_ic_func oplus_vb_overwrite_funcs[] = {
	OPLUS_IC_FUNC_BUCK_INPUT_PRESENT,
	OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND,
	OPLUS_IC_FUNC_BUCK_INPUT_IS_SUSPEND,
	OPLUS_IC_FUNC_BUCK_OUTPUT_SUSPEND,
	OPLUS_IC_FUNC_BUCK_OUTPUT_IS_SUSPEND,
	OPLUS_IC_FUNC_BUCK_SET_ICL,
	OPLUS_IC_FUNC_BUCK_GET_ICL,
	OPLUS_IC_FUNC_BUCK_SET_FCC,
	OPLUS_IC_FUNC_BUCK_SET_FV,
	OPLUS_IC_FUNC_BUCK_SET_ITERM,
	OPLUS_IC_FUNC_BUCK_SET_RECHG_VOL,
	OPLUS_IC_FUNC_BUCK_GET_INPUT_CURR,
	OPLUS_IC_FUNC_BUCK_GET_INPUT_VOL,
	OPLUS_IC_FUNC_OTG_BOOST_ENABLE,
	OPLUS_IC_FUNC_SET_OTG_BOOST_VOL,
	OPLUS_IC_FUNC_SET_OTG_BOOST_CURR_LIMIT,
	OPLUS_IC_FUNC_BUCK_AICL_ENABLE,
	OPLUS_IC_FUNC_BUCK_GET_CC_ORIENTATION,
	OPLUS_IC_FUNC_BUCK_GET_HW_DETECT,
	OPLUS_IC_FUNC_BUCK_GET_CHARGER_TYPE,
	OPLUS_IC_FUNC_BUCK_QC_DETECT_ENABLE,
	OPLUS_IC_FUNC_BUCK_SHIPMODE_ENABLE,
	OPLUS_IC_FUNC_BUCK_SET_QC_CONFIG,
	OPLUS_IC_FUNC_BUCK_SET_PD_CONFIG,
	OPLUS_IC_FUNC_WLS_BOOST_ENABLE,
	OPLUS_IC_FUNC_SET_WLS_BOOST_VOL,
	OPLUS_IC_FUNC_SET_WLS_BOOST_CURR_LIMIT,
	OPLUS_IC_FUNC_VOOCPHY_ENABLE,
	OPLUS_IC_FUNC_GET_CHARGER_CYCLE,
	OPLUS_IC_FUNC_GET_SHUTDOWN_SOC,
	OPLUS_IC_FUNC_BACKUP_SOC,
	OPLUS_IC_FUNC_BUCK_GET_VBUS_COLLAPSE_STATUS,
	OPLUS_IC_FUNC_GET_USB_TEMP_VOLT,
	OPLUS_IC_FUNC_GET_USB_TEMP,
	OPLUS_IC_FUNC_USB_TEMP_CHECK_IS_SUPPORT,
	OPLUS_IC_FUNC_GET_TYPEC_MODE,
	OPLUS_IC_FUNC_SET_TYPEC_MODE,
	OPLUS_IC_FUNC_SET_USB_DISCHG_ENABLE,
	OPLUS_IC_FUNC_GET_USB_DISCHG_STATUS,
	OPLUS_IC_FUNC_SET_OTG_SWITCH_STATUS,
	OPLUS_IC_FUNC_GET_OTG_SWITCH_STATUS,
	OPLUS_IC_FUNC_GET_OTG_ONLINE_STATUS,
	OPLUS_IC_FUNC_BUCK_WDT_ENABLE,
};

#endif /* CONFIG_OPLUS_CHG_IC_DEBUG */

static void oplus_vb_err_handler(struct oplus_chg_ic_dev *ic_dev, void *virq_data)
{
	struct oplus_virtual_buck_ic *chip = virq_data;

	oplus_chg_ic_move_err_msg(chip->ic_dev, ic_dev);
	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_ERR);
}

static void oplus_vb_cc_detect_handler(struct oplus_chg_ic_dev *ic_dev, void *virq_data)
{
	struct oplus_virtual_buck_ic *chip = virq_data;

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_CC_DETECT);
}

static void oplus_vb_plugin_handler(struct oplus_chg_ic_dev *ic_dev, void *virq_data)
{
	struct oplus_virtual_buck_ic *chip = virq_data;

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_PLUGIN);
}

static void oplus_vb_cc_changed_handler(struct oplus_chg_ic_dev *ic_dev, void *virq_data)
{
	struct oplus_virtual_buck_ic *chip = virq_data;

	oplus_chg_ic_virq_trigger(chip->ic_dev, OPLUS_IC_VIRQ_CC_CHANGED);
}

static void oplus_vb_suspend_check_handler(struct oplus_chg_ic_dev *ic_dev, void *virq_data)
{
	struct oplus_virtual_buck_ic *chip = virq_data;
	int i;

	for (i = 0; i < chip->child_num; i++) {
		if (chip->child_list[i].ic_dev != ic_dev)
			continue;
		if (!func_is_support(&chip->child_list[i], OPLUS_IC_FUNC_BUCK_SET_ICL) ||
		    chip->child_list[i].current_ratio == 0) {
			oplus_chg_ic_func(chip->child_list[i].ic_dev, OPLUS_IC_FUNC_BUCK_INPUT_SUSPEND, true);
			oplus_chg_unsuspend_plat_pmic(NULL);
		}
	}
}

struct oplus_chg_ic_virq oplus_vb_virq_table[] = {
	{ .virq_id = OPLUS_IC_VIRQ_ERR },
	{ .virq_id = OPLUS_IC_VIRQ_CC_DETECT },
	{ .virq_id = OPLUS_IC_VIRQ_PLUGIN },
	{ .virq_id = OPLUS_IC_VIRQ_CC_CHANGED },
};

static int oplus_vb_virq_register(struct oplus_virtual_buck_ic *chip)
{
	int i, rc;

	for (i = 0; i < chip->child_num; i++) {
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_ERR)) {
			rc = oplus_chg_ic_virq_register(chip->child_list[i].ic_dev,
				OPLUS_IC_VIRQ_ERR, oplus_vb_err_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_ERR error, rc=%d", rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_CC_DETECT)) {
			rc = oplus_chg_ic_virq_register(chip->child_list[i].ic_dev,
				OPLUS_IC_VIRQ_CC_DETECT, oplus_vb_cc_detect_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_CC_DETECT error, rc=%d", rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_PLUGIN)) {
			rc = oplus_chg_ic_virq_register(chip->child_list[i].ic_dev,
				OPLUS_IC_VIRQ_PLUGIN, oplus_vb_plugin_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_PLUGIN error, rc=%d", rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_CC_CHANGED)) {
			rc = oplus_chg_ic_virq_register(chip->child_list[i].ic_dev,
				OPLUS_IC_VIRQ_CC_CHANGED, oplus_vb_cc_changed_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_CC_CHANGED error, rc=%d", rc);
		}
		if (virq_is_support(&chip->child_list[i], OPLUS_IC_VIRQ_SUSPEND_CHECK)) {
			rc = oplus_chg_ic_virq_register(chip->child_list[i].ic_dev,
				OPLUS_IC_VIRQ_SUSPEND_CHECK, oplus_vb_suspend_check_handler, chip);
			if (rc < 0)
				chg_err("register OPLUS_IC_VIRQ_SUSPEND_CHECK error, rc=%d", rc);
		}
	}

	return 0;
}

static int oplus_virtual_buck_probe(struct platform_device *pdev)
{
	struct oplus_virtual_buck_ic *chip;
	struct device_node *node = pdev->dev.of_node;
	enum oplus_chg_ic_type ic_type;
	int ic_index;
	int rc = 0;

	chip = devm_kzalloc(&pdev->dev, sizeof(struct oplus_virtual_buck_ic),
			    GFP_KERNEL);
	if (chip == NULL) {
		chg_err("alloc memory error\n");
		return -ENOMEM;
	}

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	rc = oplus_vc_usbtemp_adc_init(chip);
	if (rc < 0) {
		chg_err("usbtemp adc init error, rc=%d\n", rc);
		goto usbtemp_adc_init_err;
	}
	rc = oplus_vc_usbtemp_iio_init(chip);
	if (rc < 0) {
		chg_err("usbtemp iio init error, rc=%d\n", rc);
		goto usbtemp_iio_init_err;
	}
	rc = oplus_vc_misc_init(chip);
	if (rc < 0) {
		chg_err("misic init error, rc=%d\n", rc);
		goto misic_init_err;
	}

	rc = of_property_read_u32(node, "oplus,ic_type", &ic_type);
	if (rc < 0) {
		chg_err("can't get ic type, rc=%d\n", rc);
		goto reg_ic_err;
	}
	rc = of_property_read_u32(node, "oplus,ic_index", &ic_index);
	if (rc < 0) {
		chg_err("can't get ic index, rc=%d\n", rc);
		goto reg_ic_err;
	}
	chip->ic_dev =
		devm_oplus_chg_ic_register(chip->dev, node->name, ic_index);
	if (!chip->ic_dev) {
		rc = -ENODEV;
		chg_err("register %s error\n", node->name);
		goto reg_ic_err;
	}
	sprintf(chip->ic_dev->manu_name, "virtual buck\0");
	sprintf(chip->ic_dev->fw_id, "0x00\0");
	chip->ic_dev->type = ic_type;
	chip->ic_dev->get_func = oplus_chg_vb_get_func;
	chip->ic_dev->virq_data = oplus_vb_virq_table;
	chip->ic_dev->virq_num = ARRAY_SIZE(oplus_vb_virq_table);
#ifdef CONFIG_OPLUS_CHG_IC_DEBUG
	chip->ic_dev->debug.get_func_data = oplus_chg_vb_get_func_data;
	chip->ic_dev->debug.set_func_data = oplus_chg_vb_set_func_data;
	chip->ic_dev->debug.overwrite_funcs = oplus_vb_overwrite_funcs;
	chip->ic_dev->debug.func_num = ARRAY_SIZE(oplus_vb_overwrite_funcs);
#endif

	chg_err("probe success\n");
	return 0;

reg_ic_err:
	if (oplus_vc_ccdetect_gpio_support(chip))
		gpio_free(chip->misc_gpio.ccdetect_gpio);
	if (gpio_is_valid(chip->misc_gpio.vchg_trig_gpio))
		gpio_free(chip->misc_gpio.vchg_trig_gpio);
	if (gpio_is_valid(chip->misc_gpio.ship_gpio))
		gpio_free(chip->misc_gpio.ship_gpio);
misic_init_err:
usbtemp_iio_init_err:
	if (gpio_is_valid(chip->misc_gpio.dischg_gpio))
		gpio_free(chip->misc_gpio.dischg_gpio);
usbtemp_adc_init_err:
	devm_kfree(&pdev->dev, chip);
	platform_set_drvdata(pdev, NULL);

	chg_err("probe error\n");
	return rc;
}

static int oplus_virtual_buck_remove(struct platform_device *pdev)
{
	struct oplus_virtual_buck_ic *chip = platform_get_drvdata(pdev);

	if(chip == NULL)
		return -ENODEV;

	if (chip->ic_dev->online)
		oplus_chg_vb_exit(chip->ic_dev);
	devm_oplus_chg_ic_unregister(&pdev->dev, chip->ic_dev);
	if (oplus_vc_ccdetect_gpio_support(chip))
		gpio_free(chip->misc_gpio.ccdetect_gpio);
	if (gpio_is_valid(chip->misc_gpio.vchg_trig_gpio))
		gpio_free(chip->misc_gpio.vchg_trig_gpio);
	if (gpio_is_valid(chip->misc_gpio.ship_gpio))
		gpio_free(chip->misc_gpio.ship_gpio);
	if (gpio_is_valid(chip->misc_gpio.dischg_gpio))
		gpio_free(chip->misc_gpio.dischg_gpio);
	devm_kfree(&pdev->dev, chip->child_list);
	devm_kfree(&pdev->dev, chip);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id oplus_virtual_buck_match[] = {
	{ .compatible = "oplus,virtual_buck" },
	{},
};


static struct platform_driver oplus_virtual_buck_driver = {
	.driver		= {
		.name = "oplus-virtual_buck",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(oplus_virtual_buck_match),
	},
	.probe		= oplus_virtual_buck_probe,
	.remove		= oplus_virtual_buck_remove,
};

static __init int oplus_virtual_buck_init(void)
{
	return platform_driver_register(&oplus_virtual_buck_driver);
}

static __exit void oplus_virtual_buck_exit(void)
{
	platform_driver_unregister(&oplus_virtual_buck_driver);
}

oplus_chg_module_register(oplus_virtual_buck);
