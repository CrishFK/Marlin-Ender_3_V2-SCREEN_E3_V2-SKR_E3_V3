/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "../inc/MarlinConfigPre.h"

#if ENABLED(PROBE_TEMP_COMPENSATION)

//#define DEBUG_PTC   // Print extra debug output with 'M871'

#include "probe_temp_comp.h"
#include <math.h>

ProbeTempComp temp_comp;

int16_t ProbeTempComp::z_offsets_probe[cali_info_init[TSI_PROBE].measurements],  // = {0}
        ProbeTempComp::z_offsets_bed[cali_info_init[TSI_BED].measurements];      // = {0}

#if ENABLED(USE_TEMP_EXT_COMPENSATION)
  int16_t ProbeTempComp::z_offsets_ext[cali_info_init[TSI_EXT].measurements];    // = {0}
#endif

int16_t *ProbeTempComp::sensor_z_offsets[TSI_COUNT] = {
  ProbeTempComp::z_offsets_probe, ProbeTempComp::z_offsets_bed
  OPTARG(USE_TEMP_EXT_COMPENSATION, ProbeTempComp::z_offsets_ext)
};

const temp_calib_t ProbeTempComp::cali_info[TSI_COUNT] = {
  cali_info_init[TSI_PROBE], cali_info_init[TSI_BED]
  OPTARG(USE_TEMP_EXT_COMPENSATION, cali_info_init[TSI_EXT])
};

constexpr xyz_pos_t ProbeTempComp::park_point;
constexpr xy_pos_t ProbeTempComp::measure_point;
constexpr celsius_t ProbeTempComp::probe_calib_bed_temp;

uint8_t ProbeTempComp::calib_idx; // = 0
float ProbeTempComp::init_measurement; // = 0.0

void ProbeTempComp::clear_offsets(const TempSensorID tsi) {
  LOOP_L_N(i, cali_info[tsi].measurements)
    sensor_z_offsets[tsi][i] = 0;
  calib_idx = 0;
}

bool ProbeTempComp::set_offset(const TempSensorID tsi, const uint8_t idx, const int16_t offset) {
  if (idx >= cali_info[tsi].measurements) return false;
  sensor_z_offsets[tsi][idx] = offset;
  return true;
}

void ProbeTempComp::print_offsets() {
  LOOP_L_N(s, TSI_COUNT) {
    celsius_t temp = cali_info[s].start_temp;
    for (int16_t i = -1; i < cali_info[s].measurements; ++i) {
      SERIAL_ECHOF(s == TSI_BED ? F("Bed") :
        #if ENABLED(USE_TEMP_EXT_COMPENSATION)
          s == TSI_EXT ? F("Extruder") :
        #endif
        F("Probe")
      );
      SERIAL_ECHOLNPGM(
        " temp: ", temp,
        "C; Offset: ", i < 0 ? 0.0f : sensor_z_offsets[s][i], " um"
      );
      temp += cali_info[s].temp_resolution;
    }
  }
  #if ENABLED(DEBUG_PTC)
    float meas[4] = { 0, 0, 0, 0 };
    compensate_measurement(TSI_PROBE, 27.5, meas[0]);
    compensate_measurement(TSI_PROBE, 32.5, meas[1]);
    compensate_measurement(TSI_PROBE, 77.5, meas[2]);
    compensate_measurement(TSI_PROBE, 82.5, meas[3]);
    SERIAL_ECHOLNPGM("DEBUG_PTC 27.5:", meas[0], " 32.5:", meas[1], " 77.5:", meas[2], " 82.5:", meas[3]);
  #endif
}

void ProbeTempComp::prepare_new_calibration(const_float_t init_meas_z) {
  calib_idx = 0;
  init_measurement = init_meas_z;
}

void ProbeTempComp::push_back_new_measurement(const TempSensorID tsi, const_float_t meas_z) {
  switch (tsi) {
    case TSI_PROBE:
    case TSI_BED:
    //case TSI_EXT:
      if (calib_idx >= cali_info[tsi].measurements) return;
      sensor_z_offsets[tsi][calib_idx++] = static_cast<int16_t>(meas_z * 1000.0f - init_measurement * 1000.0f);
    default: break;
  }
}

bool ProbeTempComp::finish_calibration(const TempSensorID tsi) {
  if (tsi != TSI_PROBE && tsi != TSI_BED) return false;

  if (calib_idx < 3) {
    SERIAL_ECHOLNPGM("!Insufficient measurements (min. 3).");
    clear_offsets(tsi);
    return false;
  }

  const uint8_t measurements = cali_info[tsi].measurements;
  const celsius_t start_temp = cali_info[tsi].start_temp,
                    res_temp = cali_info[tsi].temp_resolution;
  int16_t * const data = sensor_z_offsets[tsi];

  // Extrapolate
  float k, d;
  if (calib_idx < measurements) {
    SERIAL_ECHOLNPGM("Got ", calib_idx, " measurements. ");
    if (linear_regression(tsi, k, d)) {
      SERIAL_ECHOPGM("Applying linear extrapolation");
      calib_idx--;
      for (; calib_idx < measurements; ++calib_idx) {
        const celsius_float_t temp = start_temp + float(calib_idx) * res_temp;
        data[calib_idx] = static_cast<int16_t>(k * temp + d);
      }
    }
    else {
      // Simply use the last measured value for higher temperatures
      SERIAL_ECHOPGM("Failed to extrapolate");
      const int16_t last_val = data[calib_idx];
      for (; calib_idx < measurements; ++calib_idx)
        data[calib_idx] = last_val;
    }
    SERIAL_ECHOLNPGM(" for higher temperatures.");
  }

  // Sanity check
  for (calib_idx = 0; calib_idx < measurements; ++calib_idx) {
    // Restrict the max. offset
    if (ABS(data[calib_idx]) > 2000) {
      SERIAL_ECHOLNPGM("!Invalid Z-offset detected (0-2).");
      clear_offsets(tsi);
      return false;
    }
    // Restrict the max. offset difference between two probings
    if (calib_idx > 0 && ABS(data[calib_idx - 1] - data[calib_idx]) > 800) {
      SERIAL_ECHOLNPGM("!Invalid Z-offset between two probings detected (0-0.8).");
      clear_offsets(TSI_PROBE);
      return false;
    }
  }

  return true;
}

void ProbeTempComp::compensate_measurement(const TempSensorID tsi, const celsius_t temp, float &meas_z) {
  const uint8_t measurements = cali_info[tsi].measurements;
  const celsius_t start_temp = cali_info[tsi].start_temp,
                    end_temp = cali_info[tsi].end_temp,
                    res_temp = cali_info[tsi].temp_resolution;
  const int16_t * const data = sensor_z_offsets[tsi];

  // Given a data index, return { celsius, zoffset } in the form { x, y }
  auto tpoint = [&](uint8_t i) -> xy_float_t {
    return xy_float_t({ static_cast<float>(start_temp) + i * res_temp, i ? static_cast<float>(data[i - 1]) : 0.0f });
  };

  // Interpolate Z based on a temperature being within a given range
  auto linear_interp = [](const_float_t x, xy_float_t p1, xy_float_t p2) {
    //   zoffs1 +      zoffset_per_toffset      *  toffset
    return p1.y + (p2.y - p1.y) / (p2.x - p1.x) * (x - p1.x);
  };

  // offset in µm
  float offset = 0.0f;

  #if PTC_LINEAR_EXTRAPOLATION
    if (temp < start_temp)
      offset = linear_interp(temp, tpoint(0), tpoint(PTC_LINEAR_EXTRAPOLATION));
    else if (temp >= end_temp)
      offset = linear_interp(temp, tpoint(measurements - PTC_LINEAR_EXTRAPOLATION), tpoint(measurements));
  #else
    if (temp < start_temp)
      offset = 0.0f;
    else if (temp >= end_temp)
      offset = static_cast<float>(data[measurements - 1]);
  #endif
    else {
      // Linear interpolation
      const int8_t idx = static_cast<int8_t>((temp - start_temp) / res_temp);
      offset = linear_interp(temp, tpoint(idx), tpoint(idx + 1));
    }

  // convert offset to mm and apply it
  meas_z -= offset / 1000.0f;
}

bool ProbeTempComp::linear_regression(const TempSensorID tsi, float &k, float &d) {
  if (tsi != TSI_PROBE && tsi != TSI_BED) return false;

  if (!WITHIN(calib_idx, 2, cali_info[tsi].measurements)) return false;

  const celsius_t start_temp = cali_info[tsi].start_temp,
                    res_temp = cali_info[tsi].temp_resolution;
  const int16_t * const data = sensor_z_offsets[tsi];

  float sum_x = start_temp,
        sum_x2 = sq(start_temp),
        sum_xy = 0, sum_y = 0;

  float xi = static_cast<float>(start_temp);
  LOOP_L_N(i, calib_idx) {
    const float yi = static_cast<float>(data[i]);
    xi += res_temp;
    sum_x += xi;
    sum_x2 += sq(xi);
    sum_xy += xi * yi;
    sum_y += yi;
  }

  const float denom = static_cast<float>(calib_idx + 1) * sum_x2 - sq(sum_x);
  if (fabs(denom) <= 10e-5) {
    // Singularity - unable to solve
    k = d = 0.0;
    return false;
  }

  k = (static_cast<float>(calib_idx + 1) * sum_xy - sum_x * sum_y) / denom;
  d = (sum_y - k * sum_x) / static_cast<float>(calib_idx + 1);

  return true;
}

#endif // PROBE_TEMP_COMPENSATION
