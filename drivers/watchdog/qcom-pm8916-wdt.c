/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017, Advantech Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>
#include <linux/regmap.h>

#define PON_PMIC_WD_RESET_S1_TIMER	0x54
#define  PON_S1_TIMER_MASK		0x7f
#define PON_PMIC_WD_RESET_S2_TIMER	0x55
#define  PON_S2_TIMER_MASK		0x7f
#define PON_PMIC_WD_RESET_S2_CTL	0x56
#define  PON_S2_CNTL_TYPE_MASK		0x0f
#define PON_PMIC_WD_RESET_S2_CTL2	0x57
#define  PON_S2_RESET_ENABLE		BIT(7)
#define PON_PMIC_WD_RESET_PET		0x58
#define  PON_PET_WD			BIT(0)

struct qcom_pm8916_wdt {
	struct watchdog_device	wdd;
	u32			baseaddr;
	struct regmap		*regmap;
	struct notifier_block	restart_nb;
};

static inline
struct qcom_pm8916_wdt *to_qcom_pm8916_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct qcom_pm8916_wdt, wdd);
}

static int qcom_pm8916_wdt_start(struct watchdog_device *wdd)
{
	int error;
	struct qcom_pm8916_wdt *wdt = to_qcom_pm8916_wdt(wdd);

	pr_info("watchdog%d: timeout=%d\n", wdd->id, wdd->timeout);

	/* Set S1/S2 timer */
	error = regmap_update_bits(wdt->regmap,
				   wdt->baseaddr + PON_PMIC_WD_RESET_S1_TIMER,
				   PON_S1_TIMER_MASK, 0x0);
	usleep_range(200, 1000);

	error = regmap_update_bits(wdt->regmap,
				   wdt->baseaddr + PON_PMIC_WD_RESET_S2_TIMER,
				   PON_S2_TIMER_MASK, wdd->timeout);
	usleep_range(200, 1000);

	/* Enable WDog S2 timer */
	error = regmap_update_bits(wdt->regmap,
				   wdt->baseaddr + PON_PMIC_WD_RESET_S2_CTL,
				   PON_S2_CNTL_TYPE_MASK, 0x1);

	error = regmap_update_bits(wdt->regmap,
				   wdt->baseaddr + PON_PMIC_WD_RESET_S2_CTL2,
				   PON_S2_RESET_ENABLE, PON_S2_RESET_ENABLE);
	usleep_range(100, 1000);
	return error;
}

static int qcom_pm8916_wdt_stop(struct watchdog_device *wdd)
{
	struct qcom_pm8916_wdt *wdt = to_qcom_pm8916_wdt(wdd);

	int error = regmap_update_bits(wdt->regmap,
					wdt->baseaddr + PON_PMIC_WD_RESET_S2_CTL2,
					PON_S2_RESET_ENABLE, 0);
	usleep_range(100, 1000);
	return error;
}

static int qcom_pm8916_wdt_ping(struct watchdog_device *wdd)
{
	struct qcom_pm8916_wdt *wdt = to_qcom_pm8916_wdt(wdd);

	int error = regmap_update_bits(wdt->regmap,
					wdt->baseaddr + PON_PMIC_WD_RESET_PET,
					PON_PET_WD, PON_PET_WD);
	usleep_range(200, 1000);
	return error;
}

static int qcom_pm8916_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	wdd->timeout = timeout;
	return qcom_pm8916_wdt_start(wdd);
}

static const struct watchdog_ops qcom_pm8916_wdt_ops = {
	.start		= qcom_pm8916_wdt_start,
	.stop		= qcom_pm8916_wdt_stop,
	.ping		= qcom_pm8916_wdt_ping,
	.set_timeout	= qcom_pm8916_wdt_set_timeout,
	.owner		= THIS_MODULE,
};

static const struct watchdog_info qcom_pm8916_wdt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT,
	.identity	= KBUILD_MODNAME,
};

static int qcom_pm8916_wdt_restart(struct notifier_block *nb, unsigned long code,
			    void *data)
{
	struct qcom_pm8916_wdt *wdt = container_of(nb, struct qcom_pm8916_wdt, restart_nb);

	if (code == SYS_DOWN || code == SYS_HALT)
		qcom_pm8916_wdt_stop(&wdt->wdd);

	return NOTIFY_DONE;
}

static int qcom_pm8916_wdt_probe(struct platform_device *pdev)
{
	struct qcom_pm8916_wdt *wdt;
	struct device_node *np = pdev->dev.of_node;
	int ret, error;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!wdt->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(np, "reg", &wdt->baseaddr);
	if (error)
		return error;

	wdt->wdd.info = &qcom_pm8916_wdt_info;
	wdt->wdd.ops = &qcom_pm8916_wdt_ops;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = 0x7F;
	wdt->wdd.parent = &pdev->dev;

	/*
	 * If 'timeout-sec' unspecified in devicetree, assume a 30 second
	 * default, unless the max timeout is less than 30 seconds, then use
	 * the max instead.
	 */
	wdt->wdd.timeout = min(wdt->wdd.max_timeout, 30U);
	watchdog_init_timeout(&wdt->wdd, 0, &pdev->dev);

	ret = watchdog_register_device(&wdt->wdd);
	if (ret) {
		dev_err(&pdev->dev, "failed to register watchdog\n");
		return ret;
	}

	/*
	 * WDT restart notifier has priority 0 (use as a last resort)
	 */
	wdt->restart_nb.notifier_call = qcom_pm8916_wdt_restart;
	ret = register_restart_handler(&wdt->restart_nb);
	if (ret)
		dev_err(&pdev->dev, "failed to setup restart handler\n");

	platform_set_drvdata(pdev, wdt);
	dev_info(&pdev->dev, "timeout=%d sec\n", wdt->wdd.timeout);
	return 0;
}

static int qcom_pm8916_wdt_remove(struct platform_device *pdev)
{
	struct qcom_pm8916_wdt *wdt = platform_get_drvdata(pdev);

	unregister_restart_handler(&wdt->restart_nb);
	watchdog_unregister_device(&wdt->wdd);
	return 0;
}

static const struct of_device_id qcom_pm8916_wdt_of_table[] = {
	{ .compatible = "qcom,pmic-wd" },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_pm8916_wdt_of_table);

static struct platform_driver qcom_pm8916_wdt_driver = {
	.probe	= qcom_pm8916_wdt_probe,
	.remove	= qcom_pm8916_wdt_remove,
	.driver	= {
		.name		= KBUILD_MODNAME,
		.of_match_table	= qcom_pm8916_wdt_of_table,
	},
};
module_platform_driver(qcom_pm8916_wdt_driver);

MODULE_DESCRIPTION("QCOM PM8916 Watchdog Driver");
MODULE_LICENSE("GPL v2");
