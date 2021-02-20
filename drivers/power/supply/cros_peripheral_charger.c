// SPDX-License-Identifier: GPL-2.0
/*
 * Power supply driver for ChromeOS EC based Peripheral Device Charger.
 *
 * Copyright 2020 Google LLC.
 */

#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/stringify.h>
#include <linux/types.h>

#define DRV_NAME		"cros-ec-pchg"
#define PCHG_DIR_NAME		"PCHG%d"
#define PCHG_DIR_NAME_LENGTH	sizeof("PCHG" __stringify(EC_PCHG_MAX_PORTS))
#define PCHG_CACHE_UPDATE_DELAY	msecs_to_jiffies(500)

struct port_data {
	int port_number;
	char name[PCHG_DIR_NAME_LENGTH];
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	int psy_status;
	int battery_percentage;
	struct charger_data *charger;
	unsigned long last_update;
};

struct charger_data {
	struct device *dev;
	struct cros_ec_dev *ec_dev;
	struct cros_ec_device *ec_device;
	int num_registered_psy;
	struct port_data *ports[EC_PCHG_MAX_PORTS];
	struct notifier_block notifier;
};

static enum power_supply_property cros_pchg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	/*
	 * todo: Add the following.
	 *
	 * POWER_SUPPLY_PROP_TECHNOLOGY,
	 * POWER_SUPPLY_PROP_ERROR,
	 * POWER_SUPPLY_PROP_SERIAL_NUMBER,
	 *
	 * POWER_SUPPLY_PROP_ONLINE can't be used because it indicates the
	 * system is powered by AC.
	 */
};

static int cros_pchg_ec_command(const struct charger_data *charger,
				unsigned int version,
				unsigned int command,
				const void *outdata,
				unsigned int outsize,
				void *indata,
				unsigned int insize)
{
	struct cros_ec_dev *ec_dev = charger->ec_dev;
	struct cros_ec_command *msg;
	int ret;

	msg = kzalloc(sizeof(*msg) + max(outsize, insize), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->version = version;
	msg->command = ec_dev->cmd_offset + command;
	msg->outsize = outsize;
	msg->insize = insize;

	if (outsize)
		memcpy(msg->data, outdata, outsize);

	ret = cros_ec_cmd_xfer_status(charger->ec_device, msg);
	if (ret >= 0 && insize)
		memcpy(indata, msg->data, insize);

	kfree(msg);
	return ret;
}

static int cros_pchg_port_count(const struct charger_data *charger)
{
	struct ec_response_pchg_count rsp;
	int ret;

	ret = cros_pchg_ec_command(charger, 0, EC_CMD_PCHG_COUNT,
				   NULL, 0, &rsp, sizeof(rsp));
	if (ret < 0) {
		dev_warn(charger->dev,
			 "Unable to get number or ports (err:%d)\n", ret);
		return ret;
	}

	return rsp.port_count;
}

static int cros_pchg_get_status(struct port_data *port)
{
	struct charger_data *charger = port->charger;
	struct ec_params_pchg req;
	struct ec_response_pchg rsp;
	struct device *dev = charger->dev;
	int old_status = port->psy_status;
	int old_percentage = port->battery_percentage;
	int ret;

	req.port = port->port_number;
	ret = cros_pchg_ec_command(charger, 0, EC_CMD_PCHG,
				   &req, sizeof(req), &rsp, sizeof(rsp));
	if (ret < 0) {
		dev_err(dev, "Unable to get port.%d status (err:%d)\n",
			port->port_number, ret);
		return ret;
	}

	switch (rsp.state) {
	case PCHG_STATE_RESET:
	case PCHG_STATE_INITIALIZED:
	case PCHG_STATE_ENABLED:
	default:
		port->psy_status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case PCHG_STATE_DETECTED:
		port->psy_status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case PCHG_STATE_CHARGING:
		port->psy_status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case PCHG_STATE_FULL:
		port->psy_status = POWER_SUPPLY_STATUS_FULL;
		break;
	}

	port->battery_percentage = rsp.battery_percentage;

	if (port->psy_status != old_status
			|| port->battery_percentage != old_percentage)
		power_supply_changed(port->psy);

	dev_dbg(dev,
		"Port %d: state=%d battery=%d%%\n",
		port->port_number, rsp.state, rsp.battery_percentage);

	return 0;
}

static int cros_pchg_get_port_status(struct port_data *port, bool ratelimit)
{
	int ret;

	if (ratelimit &&
	    time_is_after_jiffies(port->last_update + PCHG_CACHE_UPDATE_DELAY))
		return 0;

	ret = cros_pchg_get_status(port);
	if (ret < 0)
		return ret;

	port->last_update = jiffies;

	return ret;
}

static int cros_pchg_get_prop(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct port_data *port = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CAPACITY:
		cros_pchg_get_port_status(port, true);
		break;
	default:
		break;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = port->psy_status;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = port->battery_percentage;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int cros_pchg_event(const struct charger_data *charger,
			   unsigned long host_event)
{
	int i;

	for (i = 0; i < charger->num_registered_psy; i++)
		cros_pchg_get_port_status(charger->ports[i], false);

	return NOTIFY_OK;
}

static u32 cros_get_device_event(const struct charger_data *charger)
{
	struct ec_params_device_event req;
	struct ec_response_device_event rsp;
	struct device *dev = charger->dev;
	int ret;

	req.param = EC_DEVICE_EVENT_PARAM_GET_CURRENT_EVENTS;
	ret = cros_pchg_ec_command(charger, 0, EC_CMD_DEVICE_EVENT,
				   &req, sizeof(req), &rsp, sizeof(rsp));
	if (ret < 0) {
		dev_warn(dev, "Unable to get device events (err:%d)\n", ret);
		return 0;
	}

	return rsp.event_mask;
}

static int cros_ec_notify(struct notifier_block *nb,
			  unsigned long queued_during_suspend,
			  void *data)
{
	struct cros_ec_device *ec_dev = (struct cros_ec_device *)data;
	u32 host_event = cros_ec_get_host_event(ec_dev);
	struct charger_data *charger =
			container_of(nb, struct charger_data, notifier);
	u32 device_event_mask;

	if (!host_event)
		return NOTIFY_DONE;

	if (!(host_event & EC_HOST_EVENT_MASK(EC_HOST_EVENT_DEVICE)))
		return NOTIFY_DONE;

	/*
	 * todo: Retrieve device event mask in common place
	 * (e.g. cros_ec_proto.c).
	 */
	device_event_mask = cros_get_device_event(charger);
	if (!(device_event_mask & EC_DEVICE_EVENT_MASK(EC_DEVICE_EVENT_WLC)))
		return NOTIFY_DONE;

	return cros_pchg_event(charger, host_event);
}

static int cros_pchg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_device *ec_device = ec_dev->ec_dev;
	struct power_supply_desc *psy_desc;
	struct charger_data *charger;
	struct power_supply *psy;
	struct port_data *port;
	struct notifier_block *nb;
	int num_ports;
	int ret;
	int i;

	charger = devm_kzalloc(dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->dev = dev;
	charger->ec_dev = ec_dev;
	charger->ec_device = ec_device;

	ret = cros_pchg_port_count(charger);
	if (ret <= 0) {
		/*
		 * This feature is enabled by the EC and the kernel driver is
		 * included by default for CrOS devices. Don't need to be loud
		 * since this error can be normal.
		 */
		dev_info(dev, "No peripheral charge ports (err:%d)\n", ret);
		return -ENODEV;
	}

	num_ports = ret;
	if (num_ports > EC_PCHG_MAX_PORTS) {
		dev_err(dev, "Too many peripheral charge ports (%d)\n",
			num_ports);
		return -ENOBUFS;
	}

	dev_info(dev, "%d peripheral charge ports found\n", num_ports);

	for (i = 0; i < num_ports; i++) {
		struct power_supply_config psy_cfg = {};

		port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
		if (!port)
			return -ENOMEM;

		port->charger = charger;
		port->port_number = i;
		snprintf(port->name, sizeof(port->name), PCHG_DIR_NAME, i);

		psy_desc = &port->psy_desc;
		psy_desc->name = port->name;
		psy_desc->type = POWER_SUPPLY_TYPE_BATTERY;
		psy_desc->get_property = cros_pchg_get_prop;
		psy_desc->external_power_changed = NULL;
		psy_desc->properties = cros_pchg_props;
		psy_desc->num_properties = ARRAY_SIZE(cros_pchg_props);
		psy_cfg.drv_data = port;

		psy = devm_power_supply_register_no_ws(dev, psy_desc, &psy_cfg);
		if (IS_ERR(psy)) {
			dev_err(dev, "Failed to register power supply\n");
			continue;
		}
		port->psy = psy;

		charger->ports[charger->num_registered_psy++] = port;
	}

	if (!charger->num_registered_psy)
		return -ENODEV;

	nb = &charger->notifier;
	nb->notifier_call = cros_ec_notify;
	ret = blocking_notifier_chain_register(&ec_dev->ec_dev->event_notifier,
					       nb);
	if (ret < 0)
		dev_err(dev, "Failed to register notifier (err:%d)\n", ret);

	return 0;
}

static struct platform_driver cros_pchg_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = cros_pchg_probe
};

module_platform_driver(cros_pchg_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS EC peripheral device charger");
MODULE_ALIAS("platform:" DRV_NAME);
