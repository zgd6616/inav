/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/calibration.h"
#include "common/filter.h"
#include "common/log.h"
#include "common/maths.h"
#include "common/utils.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"
#include "config/feature.h"

#include "drivers/accgyro/accgyro.h"
#include "drivers/accgyro/accgyro_mpu.h"
#include "drivers/accgyro/accgyro_mpu3050.h"
#include "drivers/accgyro/accgyro_mpu6000.h"
#include "drivers/accgyro/accgyro_mpu6050.h"
#include "drivers/accgyro/accgyro_mpu6500.h"
#include "drivers/accgyro/accgyro_mpu9250.h"

#include "drivers/accgyro/accgyro_lsm303dlhc.h"
#include "drivers/accgyro/accgyro_l3g4200d.h"
#include "drivers/accgyro/accgyro_l3gd20.h"
#include "drivers/accgyro/accgyro_adxl345.h"
#include "drivers/accgyro/accgyro_mma845x.h"
#include "drivers/accgyro/accgyro_bma280.h"
#include "drivers/accgyro/accgyro_bmi160.h"
#include "drivers/accgyro/accgyro_icm20689.h"
#include "drivers/accgyro/accgyro_fake.h"
#include "drivers/io.h"

#include "fc/config.h"
#include "fc/runtime_config.h"

#include "io/beeper.h"
#include "io/statusindicator.h"

#include "scheduler/scheduler.h"

#include "sensors/boardalignment.h"
#include "sensors/gyro.h"
#include "sensors/sensors.h"

#include "flight/gyroanalyse.h"

#ifdef USE_HARDWARE_REVISION_DETECTION
#include "hardware_revision.h"
#endif

FASTRAM gyro_t gyro; // gyro sensor object

STATIC_UNIT_TESTED gyroDev_t gyroDev0;  // Not in FASTRAM since it may hold DMA buffers
STATIC_FASTRAM int16_t gyroTemperature0;

STATIC_FASTRAM_UNIT_TESTED zeroCalibrationVector_t gyroCalibration;
STATIC_FASTRAM int32_t gyroADC[XYZ_AXIS_COUNT];

STATIC_FASTRAM filterApplyFnPtr softLpfFilterApplyFn;
STATIC_FASTRAM void *softLpfFilter[XYZ_AXIS_COUNT];

#ifdef USE_GYRO_NOTCH_1
STATIC_FASTRAM filterApplyFnPtr notchFilter1ApplyFn;
STATIC_FASTRAM void *notchFilter1[XYZ_AXIS_COUNT];
#endif
#ifdef USE_GYRO_NOTCH_2
STATIC_FASTRAM filterApplyFnPtr notchFilter2ApplyFn;
STATIC_FASTRAM void *notchFilter2[XYZ_AXIS_COUNT];
#endif

#if defined(USE_GYRO_BIQUAD_RC_FIR2)
// gyro biquad RC FIR2 filter
STATIC_FASTRAM filterApplyFnPtr gyroFilterStage2ApplyFn;
STATIC_FASTRAM void *stage2Filter[XYZ_AXIS_COUNT];
#endif

#ifdef USE_DYNAMIC_FILTERS

#define DYNAMIC_NOTCH_DEFAULT_CENTER_HZ 350
#define DYNAMIC_NOTCH_DEFAULT_CUTOFF_HZ 300

static EXTENDED_FASTRAM filterApplyFnPtr notchFilterDynApplyFn;
static EXTENDED_FASTRAM filterApplyFnPtr notchFilterDynApplyFn2;
static EXTENDED_FASTRAM biquadFilter_t notchFilterDyn[XYZ_AXIS_COUNT];
static EXTENDED_FASTRAM biquadFilter_t notchFilterDyn2[XYZ_AXIS_COUNT];
EXTENDED_FASTRAM gyroAnalyseState_t gyroAnalyseState;
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(gyroConfig_t, gyroConfig, PG_GYRO_CONFIG, 6);

PG_RESET_TEMPLATE(gyroConfig_t, gyroConfig,
    .gyro_lpf = GYRO_LPF_42HZ,      // 42HZ value is defined for Invensense/TDK gyros
    .gyro_soft_lpf_hz = 60,
    .gyro_soft_lpf_type = FILTER_BIQUAD,
    .gyro_align = ALIGN_DEFAULT,
    .gyroMovementCalibrationThreshold = 32,
    .looptime = 1000,
    .gyroSync = 1,
    .gyro_to_use = 0,
    .gyro_soft_notch_hz_1 = 0,
    .gyro_soft_notch_cutoff_1 = 1,
    .gyro_soft_notch_hz_2 = 0,
    .gyro_soft_notch_cutoff_2 = 1,
    .gyro_stage2_lowpass_hz = 0,
    .dyn_notch_width_percent = 8,
    .dyn_notch_range = DYN_NOTCH_RANGE_MEDIUM,
    .dyn_notch_q = 120,
    .dyn_notch_min_hz = 150,
);

STATIC_UNIT_TESTED gyroSensor_e gyroDetect(gyroDev_t *dev, gyroSensor_e gyroHardware)
{
    dev->gyroAlign = ALIGN_DEFAULT;

    switch (gyroHardware) {
    case GYRO_AUTODETECT:
        FALLTHROUGH;

#ifdef USE_GYRO_MPU6050
    case GYRO_MPU6050:
        if (mpu6050GyroDetect(dev)) {
            gyroHardware = GYRO_MPU6050;
#ifdef GYRO_MPU6050_ALIGN
            dev->gyroAlign = GYRO_MPU6050_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_L3G4200D
    case GYRO_L3G4200D:
        if (l3g4200dDetect(dev)) {
            gyroHardware = GYRO_L3G4200D;
#ifdef GYRO_L3G4200D_ALIGN
            dev->gyroAlign = GYRO_L3G4200D_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_MPU3050
    case GYRO_MPU3050:
        if (mpu3050Detect(dev)) {
            gyroHardware = GYRO_MPU3050;
#ifdef GYRO_MPU3050_ALIGN
            dev->gyroAlign = GYRO_MPU3050_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_L3GD20
    case GYRO_L3GD20:
        if (l3gd20Detect(dev)) {
            gyroHardware = GYRO_L3GD20;
#ifdef GYRO_L3GD20_ALIGN
            dev->gyroAlign = GYRO_L3GD20_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_MPU6000
    case GYRO_MPU6000:
        if (mpu6000GyroDetect(dev)) {
            gyroHardware = GYRO_MPU6000;
#ifdef GYRO_MPU6000_ALIGN
            dev->gyroAlign = GYRO_MPU6000_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#if defined(USE_GYRO_MPU6500)
    case GYRO_MPU6500:
        if (mpu6500GyroDetect(dev)) {
            gyroHardware = GYRO_MPU6500;
#ifdef GYRO_MPU6500_ALIGN
            dev->gyroAlign = GYRO_MPU6500_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_MPU9250
    case GYRO_MPU9250:
        if (mpu9250GyroDetect(dev)) {
            gyroHardware = GYRO_MPU9250;
#ifdef GYRO_MPU9250_ALIGN
            dev->gyroAlign = GYRO_MPU9250_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_BMI160
    case GYRO_BMI160:
        if (bmi160GyroDetect(dev)) {
            gyroHardware = GYRO_BMI160;
#ifdef GYRO_BMI160_ALIGN
            dev->gyroAlign = GYRO_BMI160_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_ICM20689
    case GYRO_ICM20689:
        if (icm20689GyroDetect(dev)) {
            gyroHardware = GYRO_ICM20689;
#ifdef GYRO_ICM20689_ALIGN
            dev->gyroAlign = GYRO_ICM20689_ALIGN;
#endif
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_FAKE_GYRO
    case GYRO_FAKE:
        if (fakeGyroDetect(dev)) {
            gyroHardware = GYRO_FAKE;
            break;
        }
        FALLTHROUGH;
#endif

    default:
    case GYRO_NONE:
        gyroHardware = GYRO_NONE;
    }

    if (gyroHardware != GYRO_NONE) {
        detectedSensors[SENSOR_INDEX_GYRO] = gyroHardware;
        sensorsSet(SENSOR_GYRO);
    }
    return gyroHardware;
}

#ifdef USE_DYNAMIC_FILTERS
bool isDynamicFilterActive(void)
{
    return feature(FEATURE_DYNAMIC_FILTERS);
}

static void gyroInitFilterDynamicNotch(void)
{

    notchFilterDynApplyFn = nullFilterApply;
    notchFilterDynApplyFn2 = nullFilterApply;

    if (isDynamicFilterActive()) {
        notchFilterDynApplyFn = (filterApplyFnPtr)biquadFilterApplyDF1; // must be this function, not DF2
        if(gyroConfig()->dyn_notch_width_percent != 0) {
            notchFilterDynApplyFn2 = (filterApplyFnPtr)biquadFilterApplyDF1; // must be this function, not DF2
        }
        const float notchQ = filterGetNotchQ(DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, DYNAMIC_NOTCH_DEFAULT_CUTOFF_HZ); // any defaults OK here
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            biquadFilterInit(&notchFilterDyn[axis], DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, getLooptime(), notchQ, FILTER_NOTCH);
            biquadFilterInit(&notchFilterDyn2[axis], DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, getLooptime(), notchQ, FILTER_NOTCH);
        }
    }

}
#endif

bool gyroInit(void)
{
    memset(&gyro, 0, sizeof(gyro));

    // Set inertial sensor tag (for dual-gyro selection)
#ifdef USE_DUAL_GYRO
    gyroDev0.imuSensorToUse = gyroConfig()->gyro_to_use;
#else
    gyroDev0.imuSensorToUse = 0;
#endif

    if (gyroDetect(&gyroDev0, GYRO_AUTODETECT) == GYRO_NONE) {
        return false;
    }

    // Driver initialisation
    gyroDev0.lpf = gyroConfig()->gyro_lpf;
    gyroDev0.requestedSampleIntervalUs = gyroConfig()->looptime;
    gyroDev0.sampleRateIntervalUs = gyroConfig()->looptime;
    gyroDev0.initFn(&gyroDev0);

    // initFn will initialize sampleRateIntervalUs to actual gyro sampling rate (if driver supports it). Calculate target looptime using that value
    gyro.targetLooptime = gyroConfig()->gyroSync ? gyroDev0.sampleRateIntervalUs : gyroConfig()->looptime;

    if (gyroConfig()->gyro_align != ALIGN_DEFAULT) {
        gyroDev0.gyroAlign = gyroConfig()->gyro_align;
    }

    gyroInitFilters();
#ifdef USE_DYNAMIC_FILTERS
    gyroInitFilterDynamicNotch();
    gyroDataAnalyseStateInit(&gyroAnalyseState, getLooptime());
#endif
    return true;
}

void gyroInitFilters(void)
{
    STATIC_FASTRAM filter_t gyroFilterLPF[XYZ_AXIS_COUNT];
    softLpfFilterApplyFn = nullFilterApply;
#ifdef USE_GYRO_NOTCH_1
    STATIC_FASTRAM biquadFilter_t gyroFilterNotch_1[XYZ_AXIS_COUNT];
    notchFilter1ApplyFn = nullFilterApply;
#endif
#ifdef USE_GYRO_NOTCH_2
    STATIC_FASTRAM biquadFilter_t gyroFilterNotch_2[XYZ_AXIS_COUNT];
    notchFilter2ApplyFn = nullFilterApply;
#endif

#ifdef USE_GYRO_BIQUAD_RC_FIR2
    STATIC_FASTRAM biquadFilter_t gyroFilterStage2[XYZ_AXIS_COUNT];
    gyroFilterStage2ApplyFn = nullFilterApply;

    if (gyroConfig()->gyro_stage2_lowpass_hz > 0) {
        gyroFilterStage2ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        for (int axis = 0; axis < 3; axis++) {
            stage2Filter[axis] = &gyroFilterStage2[axis];
            biquadRCFIR2FilterInit(stage2Filter[axis], gyroConfig()->gyro_stage2_lowpass_hz, getLooptime());
        }
    }
#endif

    if (gyroConfig()->gyro_soft_lpf_hz) {
        
        switch (gyroConfig()->gyro_soft_lpf_type) 
        {
        case FILTER_PT1:
            softLpfFilterApplyFn = (filterApplyFnPtr)pt1FilterApply;
            for (int axis = 0; axis < 3; axis++) {
                softLpfFilter[axis] = &gyroFilterLPF[axis].pt1;
                pt1FilterInit(softLpfFilter[axis], gyroConfig()->gyro_soft_lpf_hz, getLooptime()* 1e-6f);
            }
            break;
        case FILTER_BIQUAD:
            softLpfFilterApplyFn = (filterApplyFnPtr)biquadFilterApply;
            for (int axis = 0; axis < 3; axis++) {
                softLpfFilter[axis] = &gyroFilterLPF[axis].biquad;
                biquadFilterInitLPF(softLpfFilter[axis], gyroConfig()->gyro_soft_lpf_hz, getLooptime());
            }
            break;
        }
    }

#ifdef USE_GYRO_NOTCH_1
    if (gyroConfig()->gyro_soft_notch_hz_1) {
        notchFilter1ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        for (int axis = 0; axis < 3; axis++) {
            notchFilter1[axis] = &gyroFilterNotch_1[axis];
            biquadFilterInitNotch(notchFilter1[axis], getLooptime(), gyroConfig()->gyro_soft_notch_hz_1, gyroConfig()->gyro_soft_notch_cutoff_1);
        }
    }
#endif

#ifdef USE_GYRO_NOTCH_2
    if (gyroConfig()->gyro_soft_notch_hz_2) {
        notchFilter2ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        for (int axis = 0; axis < 3; axis++) {
            notchFilter2[axis] = &gyroFilterNotch_2[axis];
            biquadFilterInitNotch(notchFilter2[axis], getLooptime(), gyroConfig()->gyro_soft_notch_hz_2, gyroConfig()->gyro_soft_notch_cutoff_2);
        }
    }
#endif
}

void gyroStartCalibration(void)
{
    zeroCalibrationStartV(&gyroCalibration, CALIBRATING_GYRO_TIME_MS, gyroConfig()->gyroMovementCalibrationThreshold, false);
}

bool FAST_CODE NOINLINE gyroIsCalibrationComplete(void)
{
    return zeroCalibrationIsCompleteV(&gyroCalibration) && zeroCalibrationIsSuccessfulV(&gyroCalibration);
}

STATIC_UNIT_TESTED void performGyroCalibration(gyroDev_t *dev, zeroCalibrationVector_t *gyroCalibration)
{
    fpVector3_t v;

    // Consume gyro reading
    v.v[0] = dev->gyroADCRaw[0];
    v.v[1] = dev->gyroADCRaw[1];
    v.v[2] = dev->gyroADCRaw[2];

    zeroCalibrationAddValueV(gyroCalibration, &v);

    // Check if calibration is complete after this cycle
    if (zeroCalibrationIsCompleteV(gyroCalibration)) {
        zeroCalibrationGetZeroV(gyroCalibration, &v);
        dev->gyroZero[0] = v.v[0];
        dev->gyroZero[1] = v.v[1];
        dev->gyroZero[2] = v.v[2];

        LOG_D(GYRO, "Gyro calibration complete (%d, %d, %d)", dev->gyroZero[0], dev->gyroZero[1], dev->gyroZero[2]);
        schedulerResetTaskStatistics(TASK_SELF); // so calibration cycles do not pollute tasks statistics
    }
    else {
        dev->gyroZero[0] = 0;
        dev->gyroZero[1] = 0;
        dev->gyroZero[2] = 0;
    }
}

/*
 * Calculate rotation rate in rad/s in body frame
 */
void gyroGetMeasuredRotationRate(fpVector3_t *measuredRotationRate)
{
    for (int axis = 0; axis < 3; axis++) {
        measuredRotationRate->v[axis] = DEGREES_TO_RADIANS(gyro.gyroADCf[axis]);
    }
}

void FAST_CODE NOINLINE gyroUpdate()
{
    // range: +/- 8192; +/- 2000 deg/sec
    if (gyroDev0.readFn(&gyroDev0)) {
        if (zeroCalibrationIsCompleteV(&gyroCalibration)) {
            // Copy gyro value into int32_t (to prevent overflow) and then apply calibration and alignment
            gyroADC[X] = (int32_t)gyroDev0.gyroADCRaw[X] - (int32_t)gyroDev0.gyroZero[X];
            gyroADC[Y] = (int32_t)gyroDev0.gyroADCRaw[Y] - (int32_t)gyroDev0.gyroZero[Y];
            gyroADC[Z] = (int32_t)gyroDev0.gyroADCRaw[Z] - (int32_t)gyroDev0.gyroZero[Z];
            applySensorAlignment(gyroADC, gyroADC, gyroDev0.gyroAlign);
            applyBoardAlignment(gyroADC);
        } else {
            performGyroCalibration(&gyroDev0, &gyroCalibration);
            // Reset gyro values to zero to prevent other code from using uncalibrated data
            gyro.gyroADCf[X] = 0.0f;
            gyro.gyroADCf[Y] = 0.0f;
            gyro.gyroADCf[Z] = 0.0f;
            // still calibrating, so no need to further process gyro data
            return;
        }
    } else {
        // no gyro reading to process
        return;
    }

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        float gyroADCf = (float)gyroADC[axis] * gyroDev0.scale;

        DEBUG_SET(DEBUG_GYRO, axis, lrintf(gyroADCf));

#ifdef USE_DYNAMIC_FILTERS
        if (isDynamicFilterActive()) {
            if (axis == FD_ROLL) {
                DEBUG_SET(DEBUG_FFT, 0, lrintf(gyroADCf));
                DEBUG_SET(DEBUG_FFT_FREQ, 3, lrintf(gyroADCf));
            }
        }
#endif

        if (axis < 2) {
            DEBUG_SET(DEBUG_STAGE2, axis, lrintf(gyroADCf));
        }

#ifdef USE_GYRO_BIQUAD_RC_FIR2
        gyroADCf = gyroFilterStage2ApplyFn(stage2Filter[axis], gyroADCf);
#endif

        if (axis < 2) {
            DEBUG_SET(DEBUG_STAGE2, axis + 2, lrintf(gyroADCf));
        }

        gyroADCf = softLpfFilterApplyFn(softLpfFilter[axis], gyroADCf);

        DEBUG_SET(DEBUG_NOTCH, axis, lrintf(gyroADCf));

#ifdef USE_GYRO_NOTCH_1
        gyroADCf = notchFilter1ApplyFn(notchFilter1[axis], gyroADCf);
#endif
#ifdef USE_GYRO_NOTCH_2
        gyroADCf = notchFilter2ApplyFn(notchFilter2[axis], gyroADCf);
#endif

#ifdef USE_DYNAMIC_FILTERS
        if (isDynamicFilterActive()) {
            if (axis == FD_ROLL) {
                DEBUG_SET(DEBUG_FFT, 1, lrintf(gyroADCf));
                DEBUG_SET(DEBUG_FFT_FREQ, 2, lrintf(gyroADCf));
            }
            gyroDataAnalysePush(&gyroAnalyseState, axis, gyroADCf);
            gyroADCf = notchFilterDynApplyFn((filter_t *)&notchFilterDyn[axis], gyroADCf);
            gyroADCf = notchFilterDynApplyFn2((filter_t *)&notchFilterDyn2[axis], gyroADCf);
        }
#endif
        gyro.gyroADCf[axis] = gyroADCf;
    }

#ifdef USE_DYNAMIC_FILTERS
    if (isDynamicFilterActive()) {
        gyroDataAnalyse(&gyroAnalyseState, notchFilterDyn, notchFilterDyn2);
    }
#endif
}

bool gyroReadTemperature(void)
{
    // Read gyro sensor temperature. temperatureFn returns temperature in [degC * 10]
    if (gyroDev0.temperatureFn) {
        return gyroDev0.temperatureFn(&gyroDev0, &gyroTemperature0);
    }

    return false;
}

int16_t gyroGetTemperature(void)
{
    return gyroTemperature0;
}

int16_t gyroRateDps(int axis)
{
    return lrintf(gyro.gyroADCf[axis] / gyroDev0.scale);
}

bool gyroSyncCheckUpdate(void)
{
    if (!gyroDev0.intStatusFn)
        return false;

    return gyroDev0.intStatusFn(&gyroDev0);
}
