/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include "ssp.h"
#include <linux/math64.h>

#define BATCH_IOCTL_MAGIC		0xFC

struct batch_config {
	int64_t timeout;
	int64_t delay;
	int flag;
};

/*************************************************************************/
/* SSP data delay function                                              */
/*************************************************************************/

int get_msdelay(int64_t dDelayRate)
{
	/*
	 * From Android 5.0, There is MaxDelay Concept.
	 * If App request lower frequency then MaxDelay,
	 * Sensor have to work with MaxDelay.
	 */

	if (dDelayRate > 200000000)
		dDelayRate = 200000000;

	return div_s64(dDelayRate, 1000000);
}

static void enable_sensor(struct ssp_data *data,
	int iSensorType, int64_t dNewDelay)
{
	u8 uBuf[9];
	unsigned int uNewEnable = 0;
	s32 maxBatchReportLatency = 0;
	s8 batchOptions = 0;
	int64_t dTempDelay = data->adDelayBuf[iSensorType];
	s32 dMsDelay = get_msdelay(dNewDelay);
	int ret = 0;

	data->adDelayBuf[iSensorType] = dNewDelay;
	maxBatchReportLatency = data->batchLatencyBuf[iSensorType];
	batchOptions = data->batchOptBuf[iSensorType];

	switch (data->aiCheckStatus[iSensorType]) {
	case ADD_SENSOR_STATE:
		ssp_dbg("[SSP]: %s - add %u, New = %lldns\n",
			 __func__, 1 << iSensorType, dNewDelay);

		if (iSensorType == PROXIMITY_SENSOR) {
			get_proximity_threshold(data);
			proximity_open_calibration(data);
			set_proximity_threshold(data, data->uProxHiThresh,
				data->uProxLoThresh);
		}

#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
		if(iSensorType == LIGHT_SENSOR || iSensorType == LIGHT_IR_SENSOR) {
			data->light_log_cnt = 0;
			data->light_ir_log_cnt = 0;

		}
#else
		if(iSensorType == LIGHT_SENSOR) {
			data->light_log_cnt = 0;
		}
#endif

#ifdef CONFIG_SENSORS_SSP_SX9306
		if (iSensorType == GRIP_SENSOR) {
			open_grip_caldata(data);
			set_grip_calibration(data, true);
		}
#endif

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;

		if (iSensorType == TEMPERATURE_HUMIDITY_SENSOR &&
			dMsDelay == 10000)
			pr_info("[SSP] Skip Add Temphumidity Sensor\n");
		else
			ret = send_instruction(data, ADD_SENSOR,
				iSensorType, uBuf, 9);
		pr_info("[SSP], delay %d, timeout %d, flag=%d, ret%d \n",
			dMsDelay, maxBatchReportLatency, uBuf[8], ret);
		if (ret <= 0) {
			uNewEnable =
				(unsigned int)atomic_read(&data->aSensorEnable)
				& (~(unsigned int)(1 << iSensorType));
			atomic_set(&data->aSensorEnable, uNewEnable);

			data->aiCheckStatus[iSensorType] = NO_SENSOR_STATE;
			break;
		}

		data->aiCheckStatus[iSensorType] = RUNNING_SENSOR_STATE;
		data->bIsFirstData[iSensorType] = true;

		break;
	case RUNNING_SENSOR_STATE:
		if (get_msdelay(dTempDelay)
			== get_msdelay(data->adDelayBuf[iSensorType]))
			break;

		ssp_dbg("[SSP]: %s - Change %u, New = %lldns\n",
			__func__, 1 << iSensorType, dNewDelay);

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;
		send_instruction(data, CHANGE_DELAY, iSensorType, uBuf, 9);

		break;
	default:
		data->aiCheckStatus[iSensorType] = ADD_SENSOR_STATE;
	}
}

static void change_sensor_delay(struct ssp_data *data,
	int iSensorType, int64_t dNewDelay)
{
	u8 uBuf[9];
	s32 maxBatchReportLatency = 0;
	s8 batchOptions = 0;
	int64_t dTempDelay = data->adDelayBuf[iSensorType];
	s32 dMsDelay = get_msdelay(dNewDelay);

	data->adDelayBuf[iSensorType] = dNewDelay;
	data->batchLatencyBuf[iSensorType] = maxBatchReportLatency;
	data->batchOptBuf[iSensorType] = batchOptions;

	switch (data->aiCheckStatus[iSensorType]) {
	case RUNNING_SENSOR_STATE:
		if (get_msdelay(dTempDelay)
			== get_msdelay(data->adDelayBuf[iSensorType]))
			break;

		ssp_dbg("[SSP]: %s - Change %u, New = %lldns\n",
			__func__, 1 << iSensorType, dNewDelay);

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;
		send_instruction(data, CHANGE_DELAY, iSensorType, uBuf, 9);

		break;
	default:
		break;
	}
}

/*************************************************************************/
/* SSP data enable function                                              */
/*************************************************************************/

static int ssp_remove_sensor(struct ssp_data *data,
	unsigned int uChangedSensor, unsigned int uNewEnable)
{
	u8 uBuf[4];
	int64_t dSensorDelay = data->adDelayBuf[uChangedSensor];

	ssp_dbg("[SSP]: %s - remove sensor = %d, current state = %d\n",
		__func__, (1 << uChangedSensor), uNewEnable);

	data->adDelayBuf[uChangedSensor] = DEFUALT_POLLING_DELAY;
	data->batchLatencyBuf[uChangedSensor] = 0;
	data->batchOptBuf[uChangedSensor] = 0;

	if (uChangedSensor == ORIENTATION_SENSOR) {
		if (!(atomic_read(&data->aSensorEnable)
			& (1 << ACCELEROMETER_SENSOR))) {
			uChangedSensor = ACCELEROMETER_SENSOR;
		} else {
			change_sensor_delay(data, ACCELEROMETER_SENSOR,
				data->adDelayBuf[ACCELEROMETER_SENSOR]);
			return 0;
		}
	} else if (uChangedSensor == ACCELEROMETER_SENSOR) {
		if (atomic_read(&data->aSensorEnable)
			& (1 << ORIENTATION_SENSOR)) {
			change_sensor_delay(data, ORIENTATION_SENSOR,
				data->adDelayBuf[ORIENTATION_SENSOR]);
			return 0;
		}
	}

	if (!data->bSspShutdown)
		if (atomic_read(&data->aSensorEnable) & (1 << uChangedSensor)) {
			s32 dMsDelay = get_msdelay(dSensorDelay);
			memcpy(&uBuf[0], &dMsDelay, 4);

			send_instruction(data, REMOVE_SENSOR,
				uChangedSensor, uBuf, 4);
		}
	data->aiCheckStatus[uChangedSensor] = NO_SENSOR_STATE;

	return 0;
}

/*************************************************************************/
/* ssp Sysfs                                                             */
/*************************************************************************/

static ssize_t show_enable_irq(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	ssp_dbg("[SSP]: %s - %d\n", __func__, !data->bSspShutdown);

	return sprintf(buf, "%d\n", !data->bSspShutdown);
}

static ssize_t set_enable_irq(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	u8 dTemp;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtou8(buf, 10, &dTemp) < 0)
		return -1;

	pr_info("[SSP] %s - %d start\n", __func__, dTemp);
	if (dTemp) {
		reset_mcu(data);
		enable_debug_timer(data);
	} else if (!dTemp) {
		disable_debug_timer(data);
		ssp_enable(data, 0);
	} else
		pr_err("[SSP] %s - invalid value\n", __func__);
	pr_info("[SSP] %s - %d end\n", __func__, dTemp);
	return size;
}

static ssize_t show_sensors_enable(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	ssp_dbg("[SSP]: %s - cur_enable = %d\n", __func__,
		 atomic_read(&data->aSensorEnable));

	return sprintf(buf, "%9u\n", atomic_read(&data->aSensorEnable));
}

static ssize_t set_sensors_enable(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dTemp;
	int iRet;
	unsigned int uNewEnable = 0, uChangedSensor = 0;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dTemp) < 0)
		return -EINVAL;

	uNewEnable = (unsigned int)dTemp;
	ssp_dbg("[SSP]: %s - new_enable = %u, old_enable = %u\n", __func__,
		 uNewEnable, atomic_read(&data->aSensorEnable));

	if ((uNewEnable != atomic_read(&data->aSensorEnable)) &&
		!(data->uSensorState & (uNewEnable - atomic_read(&data->aSensorEnable)))) {
		pr_info("[SSP] %s - %u is not connected(sensortate: 0x%x)\n",
			__func__, uNewEnable - atomic_read(&data->aSensorEnable), data->uSensorState);
		return -EINVAL;
	}

	if (uNewEnable == atomic_read(&data->aSensorEnable))
		return size;

	mutex_lock(&data->enable_mutex);
	for (uChangedSensor = 0; uChangedSensor < SENSOR_MAX; uChangedSensor++) {
		if ((atomic_read(&data->aSensorEnable) & (1 << uChangedSensor))
			!= (uNewEnable & (1 << uChangedSensor))) {

			if (!(uNewEnable & (1 << uChangedSensor))) {
				data->reportedData[uChangedSensor] = false;

				/* Intterupt Gyro */
				if (uChangedSensor == INTERRUPT_GYRO_SENSOR) {
					if (!atomic_read(&data->int_gyro_enable)) {
						pr_info("[SSP] skip removing int_gyrosensor");
						continue;
					}
				}

				ssp_remove_sensor(data, uChangedSensor,
					uNewEnable); /* disable */
			} else { /* Change to ADD_SENSOR_STATE from KitKat */
				if (data->aiCheckStatus[uChangedSensor] == INITIALIZATION_STATE) {
					if (uChangedSensor == ACCELEROMETER_SENSOR) {
						accel_open_calibration(data);
						iRet = set_accel_cal(data);
						if (iRet < 0)
							pr_err("[SSP]: %s - set_accel_cal failed %d\n", __func__, iRet);
					}
					else if (uChangedSensor == PRESSURE_SENSOR)
						pressure_open_calibration(data);
					else if (uChangedSensor == PROXIMITY_SENSOR) {
						get_proximity_threshold(data);
						proximity_open_calibration(data);
						set_proximity_threshold(data, data->uProxHiThresh, data->uProxLoThresh);
					}
#ifdef CONFIG_SENSORS_SSP_SX9306
					else if (uChangedSensor == GRIP_SENSOR) {
						open_grip_caldata(data);
						set_grip_calibration(data, true);
					}
#endif
				}
				data->aiCheckStatus[uChangedSensor] = ADD_SENSOR_STATE;

				/* Intterupt Gyro */
				if (uChangedSensor == INTERRUPT_GYRO_SENSOR) {
					if (!atomic_read(&data->int_gyro_enable)) {
						pr_info("[SSP] skip enabling int_gyrosensor");
						continue;
					}
				}

				enable_sensor(data, uChangedSensor, data->adDelayBuf[uChangedSensor]);
			}
			break;
		}
	}
	atomic_set(&data->aSensorEnable, uNewEnable);
	mutex_unlock(&data->enable_mutex);

	return size;
}

static ssize_t set_flush(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dTemp;
	u8 sensor_type = 0;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dTemp) < 0)
		return -EINVAL;

	sensor_type = (u8)dTemp;
	if (!(atomic_read(&data->aSensorEnable) & (1 << sensor_type)))
		return -EINVAL;

	/* Intterupt Gyro */
	if (sensor_type == INTERRUPT_GYRO_SENSOR) {
		if (!atomic_read(&data->int_gyro_enable)) {
			data->aiCheckStatus[INTERRUPT_GYRO_SENSOR]
				= ADD_SENSOR_STATE;
			enable_sensor(data, INTERRUPT_GYRO_SENSOR,
				data->adDelayBuf[INTERRUPT_GYRO_SENSOR]);
		}
	}

	if (flush(data, sensor_type) < 0) {
		pr_err("[SSP] ssp returns error for flush(%x)\n", sensor_type);
		return -EINVAL;
	}

	/* Intterupt Gyro */
	if (sensor_type == INTERRUPT_GYRO_SENSOR) {
		if (!atomic_read(&data->int_gyro_enable)) {
			int64_t delay = data->adDelayBuf[INTERRUPT_GYRO_SENSOR];
			ssp_remove_sensor(data,	INTERRUPT_GYRO_SENSOR,
					atomic64_read(&data->aSensorEnable));
			data->adDelayBuf[INTERRUPT_GYRO_SENSOR] = delay;
		}
	}

	return size;
}

static ssize_t show_shake_cam(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);
	int enabled = 0;

	if ((atomic_read(&data->aSensorEnable) & (1 << SHAKE_CAM)))
		enabled = 1;

	return sprintf(buf, "%d\n", enabled);
}

static ssize_t set_shake_cam(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct ssp_data *data = dev_get_drvdata(dev);
	unsigned long enable = 0;
	unsigned char buffer[4];
	unsigned int uNewEnable;
	s32 dMsDelay = 20;

	if (strict_strtoul(buf, 10, &enable))
		return -EINVAL;

	memcpy(&buffer[0], &dMsDelay, 4);

	if (enable) {
		send_instruction(data, ADD_SENSOR, SHAKE_CAM,
				buffer, sizeof(buffer));
		uNewEnable =
			(unsigned int)atomic_read(&data->aSensorEnable)
			| ((unsigned int)(1 << SHAKE_CAM));
	} else {
		send_instruction(data, REMOVE_SENSOR, SHAKE_CAM,
				buffer, sizeof(buffer));
		uNewEnable =
			(unsigned int)atomic_read(&data->aSensorEnable)
			& (~(unsigned int)(1 << SHAKE_CAM));
	}

	atomic_set(&data->aSensorEnable, uNewEnable);
	return size;
}

static ssize_t show_acc_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[ACCELEROMETER_SENSOR]);
}

static ssize_t set_acc_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	if ((atomic_read(&data->aSensorEnable) & (1 << ORIENTATION_SENSOR)) &&
		(data->adDelayBuf[ORIENTATION_SENSOR] < dNewDelay))
		data->adDelayBuf[ACCELEROMETER_SENSOR] = dNewDelay;
	else
		change_sensor_delay(data, ACCELEROMETER_SENSOR, dNewDelay);

	return size;
}

static ssize_t show_gyro_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[GYROSCOPE_SENSOR]);
}

static ssize_t set_gyro_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GYROSCOPE_SENSOR, dNewDelay);
	return size;
}

#ifdef CONFIG_SENSORS_SSP_INTERRUPT_GYRO_SENSOR
static ssize_t show_interrupt_gyro_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[INTERRUPT_GYRO_SENSOR]);
}

static ssize_t set_interrupt_gyro_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, INTERRUPT_GYRO_SENSOR, dNewDelay);
	return size;
}
#endif

static ssize_t show_mag_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[GEOMAGNETIC_SENSOR]);
}

static ssize_t set_mag_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GEOMAGNETIC_SENSOR, dNewDelay);

	return size;
}

static ssize_t show_uncal_mag_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
			data->adDelayBuf[GEOMAGNETIC_UNCALIB_SENSOR]);
}

static ssize_t set_uncal_mag_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GEOMAGNETIC_UNCALIB_SENSOR, dNewDelay);

	return size;
}

static ssize_t show_rot_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[ROTATION_VECTOR]);
}

static ssize_t set_rot_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, ROTATION_VECTOR, dNewDelay);

	return size;
}

static ssize_t show_game_rot_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[GAME_ROTATION_VECTOR]);
}

static ssize_t set_game_rot_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GAME_ROTATION_VECTOR, dNewDelay);

	return size;
}

static ssize_t show_step_det_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[STEP_DETECTOR]);
}

static ssize_t set_step_det_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -1;

	change_sensor_delay(data, STEP_DETECTOR, dNewDelay);
	return size;
}

static ssize_t show_sig_motion_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[SIG_MOTION_SENSOR]);
}

static ssize_t set_sig_motion_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -1;

	change_sensor_delay(data, SIG_MOTION_SENSOR, dNewDelay);
	return size;
}

static ssize_t show_step_cnt_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[STEP_COUNTER]);
}

static ssize_t set_step_cnt_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -1;

	change_sensor_delay(data, STEP_COUNTER, dNewDelay);
	return size;
}

static ssize_t show_uncalib_gyro_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[GYRO_UNCALIB_SENSOR]);
}

static ssize_t set_uncalib_gyro_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GYRO_UNCALIB_SENSOR, dNewDelay);

	return size;
}

static ssize_t show_pressure_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[PRESSURE_SENSOR]);
}

static ssize_t set_pressure_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, PRESSURE_SENSOR, dNewDelay);
	return size;
}

static ssize_t show_gesture_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[GESTURE_SENSOR]);
}

static ssize_t set_gesture_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, GESTURE_SENSOR, dNewDelay);

	return size;
}

static ssize_t show_light_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[LIGHT_SENSOR]);
}

static ssize_t set_light_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, LIGHT_SENSOR, dNewDelay);
	return size;
}

#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
static ssize_t show_light_ir_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[LIGHT_IR_SENSOR]);
}

static ssize_t set_light_ir_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, LIGHT_IR_SENSOR, dNewDelay);
	return size;
}
#endif

static ssize_t show_prox_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n", data->adDelayBuf[PROXIMITY_SENSOR]);
}

static ssize_t set_prox_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, PROXIMITY_SENSOR, dNewDelay);
	return size;
}

static ssize_t show_temp_humi_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[TEMPERATURE_HUMIDITY_SENSOR]);
}

static ssize_t set_temp_humi_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -EINVAL;

	change_sensor_delay(data, TEMPERATURE_HUMIDITY_SENSOR, dNewDelay);
	return size;
}

static ssize_t show_tilt_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[TILT_DETECTOR]);
}

static ssize_t set_tilt_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -1;

	change_sensor_delay(data, TILT_DETECTOR, dNewDelay);
	return size;
}

static ssize_t show_pickup_delay(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);

	return sprintf(buf, "%lld\n",
		data->adDelayBuf[PICKUP_GESTURE]);
}

static ssize_t set_pickup_delay(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int64_t dNewDelay;
	struct ssp_data *data  = dev_get_drvdata(dev);

	if (kstrtoll(buf, 10, &dNewDelay) < 0)
		return -1;

	change_sensor_delay(data, PICKUP_GESTURE, dNewDelay);
	return size;
}

ssize_t ssp_sensorhub_voicel_pcmdump_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);
	int status = ssp_sensorhub_pcm_dump(data->hub_data);

	return sprintf(buf, "%s\n", (status ? "OK" : "NG"));
}

static DEVICE_ATTR(voice_pcmdump, S_IRUGO,
	ssp_sensorhub_voicel_pcmdump_show, NULL);

static struct device_attribute *voice_attrs[] = {
	&dev_attr_voice_pcmdump,
	NULL,
};

static void initialize_voice_sysfs(struct ssp_data *data)
{
	sensors_register(data->voice_device, data, voice_attrs, "ssp_voice");
}

static void remove_voice_sysfs(struct ssp_data *data)
{
	sensors_unregister(data->voice_device, voice_attrs);
}


static ssize_t show_int_gyro_enable(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d,%d\n",
		atomic_read(&data->int_gyro_enable),
		atomic_read(&data->aSensorEnable)
		& (1 << INTERRUPT_GYRO_SENSOR));
}

static ssize_t set_int_gyro_enable(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct ssp_data *data  = dev_get_drvdata(dev);
	int64_t buffer;
	bool int_gyro_enable;

	if (kstrtoll(buf, 10, &buffer) < 0)
		return -EINVAL;

	if (buffer != 1 && buffer != 0)
		return -EINVAL;

	int_gyro_enable = (bool)buffer;

	if (atomic_read(&data->int_gyro_enable) == int_gyro_enable)
		return size;

	if (atomic_read(&data->aSensorEnable)
			& (1 << INTERRUPT_GYRO_SENSOR)) {
		if (int_gyro_enable) {
			data->aiCheckStatus[INTERRUPT_GYRO_SENSOR]
				= ADD_SENSOR_STATE;
			enable_sensor(data, INTERRUPT_GYRO_SENSOR,
				data->adDelayBuf[INTERRUPT_GYRO_SENSOR]);
		} else {
			int64_t delay = data->adDelayBuf[INTERRUPT_GYRO_SENSOR];
			ssp_remove_sensor(data,	INTERRUPT_GYRO_SENSOR,
					atomic_read(&data->aSensorEnable));
			data->adDelayBuf[INTERRUPT_GYRO_SENSOR] = delay;
		}
	}

	atomic_set(&data->int_gyro_enable, int_gyro_enable);
	return size;
}

static ssize_t show_sensor_state(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data  = dev_get_drvdata(dev);
	return sprintf(buf, "%s\n", data->sensor_state);
}


static DEVICE_ATTR(mcu_rev, S_IRUGO, mcu_revision_show, NULL);
static DEVICE_ATTR(mcu_name, S_IRUGO, mcu_model_name_show, NULL);
static DEVICE_ATTR(mcu_update, S_IRUGO, mcu_update_kernel_bin_show, NULL);
static DEVICE_ATTR(mcu_update2, S_IRUGO,
	mcu_update_kernel_crashed_bin_show, NULL);
static DEVICE_ATTR(mcu_update_ums, S_IRUGO, mcu_update_ums_bin_show, NULL);
static DEVICE_ATTR(mcu_reset, S_IRUGO, mcu_reset_show, NULL);
static DEVICE_ATTR(mcu_dump, S_IRUGO, mcu_dump_show, NULL);

static DEVICE_ATTR(mcu_test, S_IRUGO | S_IWUSR | S_IWGRP,
	mcu_factorytest_show, mcu_factorytest_store);
static DEVICE_ATTR(mcu_sleep_test, S_IRUGO | S_IWUSR | S_IWGRP,
	mcu_sleep_factorytest_show, mcu_sleep_factorytest_store);
static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
	show_sensors_enable, set_sensors_enable);
static DEVICE_ATTR(enable_irq, S_IRUGO | S_IWUSR | S_IWGRP,
	show_enable_irq, set_enable_irq);
static DEVICE_ATTR(accel_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_acc_delay, set_acc_delay);
static DEVICE_ATTR(gyro_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_gyro_delay, set_gyro_delay);
static DEVICE_ATTR(uncalib_gyro_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_uncalib_gyro_delay, set_uncalib_gyro_delay);
static DEVICE_ATTR(mag_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_mag_delay, set_mag_delay);
static DEVICE_ATTR(uncal_mag_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_uncal_mag_delay, set_uncal_mag_delay);
static DEVICE_ATTR(rot_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_rot_delay, set_rot_delay);
static DEVICE_ATTR(game_rot_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_game_rot_delay, set_game_rot_delay);
static DEVICE_ATTR(step_det_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_step_det_delay, set_step_det_delay);
static DEVICE_ATTR(pressure_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_pressure_delay, set_pressure_delay);
static DEVICE_ATTR(ssp_flush, S_IWUSR | S_IWGRP,
	NULL, set_flush);
static DEVICE_ATTR(shake_cam, S_IRUGO | S_IWUSR | S_IWGRP,
	show_shake_cam, set_shake_cam);
static DEVICE_ATTR(tilt_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_tilt_delay, set_tilt_delay);
static DEVICE_ATTR(pickup_poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_pickup_delay, set_pickup_delay);
static struct device_attribute dev_attr_gesture_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_gesture_delay, set_gesture_delay);
static struct device_attribute dev_attr_light_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_light_delay, set_light_delay);
#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
static struct device_attribute dev_attr_light_ir_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_light_ir_delay, set_light_ir_delay);
#endif
#ifdef CONFIG_SENSORS_SSP_INTERRUPT_GYRO_SENSOR
static struct device_attribute dev_attr_interrupt_gyro_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_interrupt_gyro_delay, set_interrupt_gyro_delay);
#endif
static struct device_attribute dev_attr_prox_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_prox_delay, set_prox_delay);
static struct device_attribute dev_attr_temp_humi_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_temp_humi_delay, set_temp_humi_delay);
static struct device_attribute dev_attr_sig_motion_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_sig_motion_delay, set_sig_motion_delay);
static struct device_attribute dev_attr_step_cnt_poll_delay
	= __ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
	show_step_cnt_delay, set_step_cnt_delay);
static DEVICE_ATTR(int_gyro_enable, S_IRUGO | S_IWUSR | S_IWGRP,
	show_int_gyro_enable, set_int_gyro_enable);
static DEVICE_ATTR(sensor_state, S_IRUGO, show_sensor_state, NULL);

static struct device_attribute *mcu_attrs[] = {
	&dev_attr_enable,
	&dev_attr_mcu_rev,
	&dev_attr_mcu_name,
	&dev_attr_mcu_test,
	&dev_attr_mcu_reset,
	&dev_attr_mcu_dump,
	&dev_attr_mcu_update,
	&dev_attr_mcu_update2,
	&dev_attr_mcu_update_ums,
	&dev_attr_mcu_sleep_test,
	&dev_attr_enable_irq,
	&dev_attr_accel_poll_delay,
	&dev_attr_gyro_poll_delay,
	&dev_attr_uncalib_gyro_poll_delay,
	&dev_attr_mag_poll_delay,
	&dev_attr_uncal_mag_poll_delay,
	&dev_attr_rot_poll_delay,
	&dev_attr_game_rot_poll_delay,
	&dev_attr_step_det_poll_delay,
	&dev_attr_pressure_poll_delay,
	&dev_attr_tilt_poll_delay,
	&dev_attr_pickup_poll_delay,
	&dev_attr_ssp_flush,
	&dev_attr_shake_cam,
	&dev_attr_int_gyro_enable,
	&dev_attr_sensor_state,
	NULL,
};

static long ssp_batch_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct ssp_data *data
		= container_of(file->private_data,
			struct ssp_data, batch_io_device);

	struct batch_config batch;

	void __user *argp = (void __user *)arg;
	int retries = 2;
	int ret = 0;
	int sensor_type, ms_delay;
	int timeout_ms = 0;
	u8 uBuf[9];

	sensor_type = (cmd & 0xFF);

	if(sensor_type >= SENSOR_MAX){
		pr_err("[SSP] Invalid sensor_type %d\n", sensor_type);
		return -EINVAL;
	}

	if ((cmd >> 8 & 0xFF) != BATCH_IOCTL_MAGIC) {
		pr_err("[SSP] Invalid BATCH CMD %x\n", cmd);
		return -EINVAL;
	}

	while (retries--) {
		ret = copy_from_user(&batch, argp, sizeof(batch));
		if (likely(!ret))
			break;
	}
	if (unlikely(ret)) {
		pr_err("[SSP] batch ioctl err(%d)\n", ret);
		return -EINVAL;
	}
	ms_delay = get_msdelay(batch.delay);
	timeout_ms = div_s64(batch.timeout, 1000000);
	memcpy(&uBuf[0], &ms_delay, 4);
	memcpy(&uBuf[4], &timeout_ms, 4);
	uBuf[8] = batch.flag;

	if (batch.timeout){ /* add or dry */

		if(!(batch.flag & SENSORS_BATCH_DRY_RUN)) { /* real batch, NOT DRY, change delay */
			ret = 1;
			/* if sensor is not running state, enable will be called.
			   MCU return fail when receive chage delay inst during NO_SENSOR STATE */
			if (data->aiCheckStatus[sensor_type] == RUNNING_SENSOR_STATE) {
				ret = send_instruction_sync(data, CHANGE_DELAY, sensor_type, uBuf, 9);
			}
			if (ret > 0) { // ret 1 is success
				data->batchOptBuf[sensor_type] = (u8)batch.flag;
				data->batchLatencyBuf[sensor_type] = timeout_ms;
				data->adDelayBuf[sensor_type] = batch.delay;
			}
		} else { /* real batch, DRY RUN */
			ret = send_instruction_sync(data, CHANGE_DELAY, sensor_type, uBuf, 9);
			if (ret > 0) { // ret 1 is success
				data->batchOptBuf[sensor_type] = (u8)batch.flag;
				data->batchLatencyBuf[sensor_type] = timeout_ms;
				data->adDelayBuf[sensor_type] = batch.delay;
			}
		}
	} else { /* remove batch or normal change delay, remove or add will be called. */

		if (!(batch.flag & SENSORS_BATCH_DRY_RUN)) { /* no batch, NOT DRY, change delay */
			data->batchOptBuf[sensor_type] = 0;
			data->batchLatencyBuf[sensor_type] = 0;
			data->adDelayBuf[sensor_type] = batch.delay;
			if (data->aiCheckStatus[sensor_type] == RUNNING_SENSOR_STATE) {
				send_instruction(data, CHANGE_DELAY, sensor_type, uBuf, 9);
			}
		}
	}

	pr_info("[SSP] batch %d: delay %lld, timeout %lld, flag %d, ret %d \n",
		sensor_type, batch.delay, batch.timeout, batch.flag, ret);
	if (!batch.timeout)
		return 0;
	if (ret <= 0)
		return -EINVAL;
	else
		return 0;
}


static struct file_operations ssp_batch_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.unlocked_ioctl = ssp_batch_ioctl,
};

static void initialize_mcu_factorytest(struct ssp_data *data)
{
	sensors_register(data->mcu_device, data, mcu_attrs, "ssp_sensor");
}

static void remove_mcu_factorytest(struct ssp_data *data)
{
	sensors_unregister(data->mcu_device, mcu_attrs);
}

int initialize_sysfs(struct ssp_data *data)
{
	if (device_create_file(&data->gesture_input_dev->dev,
		&dev_attr_gesture_poll_delay))
		goto err_gesture_input_dev;

	if (device_create_file(&data->light_input_dev->dev,
		&dev_attr_light_poll_delay))
		goto err_light_input_dev;
#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
	if (device_create_file(&data->light_ir_input_dev->dev,
		&dev_attr_light_ir_poll_delay))
		goto err_light_ir_input_dev;
#endif
#ifdef CONFIG_SENSORS_SSP_INTERRUPT_GYRO_SENSOR
	if (device_create_file(&data->interrupt_gyro_input_dev->dev,
		&dev_attr_interrupt_gyro_poll_delay))
		goto err_interrupt_gyro_input_dev;
#endif
	if (device_create_file(&data->prox_input_dev->dev,
		&dev_attr_prox_poll_delay))
		goto err_prox_input_dev;

	if (device_create_file(&data->temp_humi_input_dev->dev,
		&dev_attr_temp_humi_poll_delay))
		goto err_temp_humi_input_dev;

	if (device_create_file(&data->sig_motion_input_dev->dev,
		&dev_attr_sig_motion_poll_delay))
		goto err_sig_motion_input_dev;

	if (device_create_file(&data->step_cnt_input_dev->dev,
		&dev_attr_step_cnt_poll_delay))
		goto err_step_cnt_input_dev;

	data->batch_io_device.minor = MISC_DYNAMIC_MINOR;
	data->batch_io_device.name = "batch_io";
	data->batch_io_device.fops = &ssp_batch_fops;
	if (misc_register(&data->batch_io_device))
		goto err_batch_io_dev;

	initialize_accel_factorytest(data);
	initialize_gyro_factorytest(data);
	initialize_prox_factorytest(data);
	initialize_light_factorytest(data);
	initialize_pressure_factorytest(data);
	initialize_magnetic_factorytest(data);
	initialize_mcu_factorytest(data);
	initialize_gesture_factorytest(data);
#ifdef CONFIG_SENSORS_SSP_TMD4903
	initialize_irled_factorytest(data);
#endif
#ifdef CONFIG_SENSORS_SSP_SHTC1
	initialize_temphumidity_factorytest(data);
#endif
#ifdef CONFIG_SENSORS_SSP_MOBEAM
	initialize_mobeam(data);
#endif
#ifdef CONFIG_SENSORS_SSP_SX9306
	initialize_grip_factorytest(data);
#endif
	/*snamy.jeong_0630 voice dump & data*/
	initialize_voice_sysfs(data);

	return SUCCESS;
err_batch_io_dev:
	device_remove_file(&data->step_cnt_input_dev->dev,
		&dev_attr_step_cnt_poll_delay);
err_step_cnt_input_dev:
	device_remove_file(&data->sig_motion_input_dev->dev,
		&dev_attr_sig_motion_poll_delay);
err_sig_motion_input_dev:
	device_remove_file(&data->temp_humi_input_dev->dev,
		&dev_attr_temp_humi_poll_delay);
err_temp_humi_input_dev:
	device_remove_file(&data->prox_input_dev->dev,
		&dev_attr_prox_poll_delay);
err_prox_input_dev:
#ifdef CONFIG_SENSORS_SSP_INTERRUPT_GYRO_SENSOR
    device_remove_file(&data->interrupt_gyro_input_dev->dev,
        &dev_attr_interrupt_gyro_poll_delay);
err_interrupt_gyro_input_dev:
#endif
#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
	device_remove_file(&data->light_ir_input_dev->dev,
		&dev_attr_light_ir_poll_delay);
err_light_ir_input_dev:
#endif
	device_remove_file(&data->light_input_dev->dev,
		&dev_attr_light_poll_delay);
err_light_input_dev:
	device_remove_file(&data->gesture_input_dev->dev,
		&dev_attr_gesture_poll_delay);
err_gesture_input_dev:
	pr_err("[SSP] error init sysfs\n");
	return ERROR;
}

void remove_sysfs(struct ssp_data *data)
{
	device_remove_file(&data->gesture_input_dev->dev,
		&dev_attr_gesture_poll_delay);
	device_remove_file(&data->light_input_dev->dev,
		&dev_attr_light_poll_delay);
#ifdef CONFIG_SENSORS_SSP_IRDATA_FOR_CAMERA
	device_remove_file(&data->light_ir_input_dev->dev,
		&dev_attr_light_ir_poll_delay);
#endif
#ifdef CONFIG_SENSORS_SSP_INTERRUPT_GYRO_SENSOR
	device_remove_file(&data->interrupt_gyro_input_dev->dev,
		&dev_attr_interrupt_gyro_poll_delay);
#endif
	device_remove_file(&data->prox_input_dev->dev,
		&dev_attr_prox_poll_delay);
	device_remove_file(&data->temp_humi_input_dev->dev,
		&dev_attr_temp_humi_poll_delay);
	device_remove_file(&data->sig_motion_input_dev->dev,
		&dev_attr_sig_motion_poll_delay);
	device_remove_file(&data->step_cnt_input_dev->dev,
		&dev_attr_step_cnt_poll_delay);
	ssp_batch_fops.unlocked_ioctl = NULL;
	misc_deregister(&data->batch_io_device);
	remove_accel_factorytest(data);
	remove_gyro_factorytest(data);
	remove_prox_factorytest(data);
	remove_light_factorytest(data);
	remove_pressure_factorytest(data);
	remove_magnetic_factorytest(data);
	remove_mcu_factorytest(data);
	remove_gesture_factorytest(data);
#ifdef CONFIG_SENSORS_SSP_TMD4903
	remove_irled_factorytest(data);
#endif
#ifdef CONFIG_SENSORS_SSP_SHTC1
	remove_temphumidity_factorytest(data);
#endif
#ifdef CONFIG_SENSORS_SSP_MOBEAM
	remove_mobeam(data);
#endif
#ifdef CONFIG_SENSORS_SSP_SX9306
	remove_grip_factorytest(data);
#endif
	/*snamy.jeong_0630 voice dump & data*/
	remove_voice_sysfs(data);

	destroy_sensor_class();
}
