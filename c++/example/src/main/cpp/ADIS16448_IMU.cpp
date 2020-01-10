/*----------------------------------------------------------------------------*/
/* Copyright (c) 2016-2018 FIRST. All Rights Reserved.                        */
/* Open Source Software - may be modified and shared by FRC teams. The code   */
/* must be accompanied by the FIRST BSD license file in the root directory of */
/* the project.                                                               */
/*                                                                            */
/* Modified by Juan Chong - juan.chong@analog.com                             */
/*----------------------------------------------------------------------------*/

#include <string>
#include <iostream>
#include <cmath>

#include <frc/DigitalInput.h>
#include <frc/DigitalSource.h>
#include <frc/DriverStation.h>
#include <frc/DigitalOutput.h>
#include <frc/DigitalSource.h>
#include <frc/DigitalInput.h>
#include <frc/SPI.h>
#include <frc/ErrorBase.h>
#include <adi/ADIS16448_IMU.h>
#include <frc/smartdashboard/SendableBuilder.h>
#include <frc/Timer.h>
#include <frc/WPIErrors.h>
#include <hal/HAL.h>

/* Helpful conversion functions */
static inline int32_t ToInt(const uint32_t *buf){
  return (int32_t)( (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3] );
}

static inline uint16_t BuffToUShort(const uint32_t* buf) {
  return ((uint16_t)(buf[0]) << 8) | buf[1];
}

static inline uint8_t BuffToUByte(const uint32_t* buf) {
  return ((uint8_t)buf[0]);
}

static inline int16_t BuffToShort(const uint32_t* buf) {
  return ((int16_t)(buf[0]) << 8) | buf[1];
}

static inline uint16_t ToUShort(const uint8_t* buf) {
  return ((uint16_t)(buf[0]) << 8) | buf[1];
}

static inline int16_t ToShort(const uint8_t* buf) {
  return (int16_t)(((uint16_t)(buf[0]) << 8) | buf[1]);
}

using namespace frc;

ADIS16448_IMU::ADIS16448_IMU() : ADIS16448_IMU(kZ, SPI::Port::kMXP, 4) {}

ADIS16448_IMU::ADIS16448_IMU(IMUAxis yaw_axis, SPI::Port port, uint16_t cal_time) : 
                m_yaw_axis(yaw_axis), 
                m_spi_port(port),
                m_calibration_time(cal_time) {

  // Force the IMU reset pin to toggle on startup (doesn't require DS enable)
  // Relies on the RIO hardware by default configuring an output as low
  // and configuring an input as high Z. The 10k pull-up resistor internal to the
  // IMU then forces the reset line high for normal operation.
  DigitalOutput *m_reset_out = new DigitalOutput(18);  // Drive MXP DIO8 low
  Wait(0.01);  // Wait 10ms
  delete m_reset_out;
  new DigitalInput(18);  // Set MXP DIO8 high
  Wait(0.5);  // Wait 500ms for reset to complete

  // Configure standard SPI
  if (!SwitchToStandardSPI()) {
    return;
  }

  // Set IMU internal decimation to 819.2 SPS
  WriteRegister(SMPL_PRD, 0x0001);
  // Enable Data Ready (LOW = Good Data) on DIO1 (PWM0 on MXP), PoP, and G sensitivity compensation
  WriteRegister(MSC_CTRL, 0x0016);
  // Configure IMU internal Bartlett filter
  WriteRegister(SENS_AVG, 0x0402);

  //TODO: Move this somewhere useful
  // Notify DS that IMU calibration delay is active
  DriverStation::ReportWarning("ADIS16448 IMU Detected. Starting initial calibration delay.");

  // Configure and enable auto SPI
  if(!SwitchToAutoSPI()) {
    return;
  }

  // Let the user know the IMU was initiallized successfully
  DriverStation::ReportWarning("ADIS16448 IMU Successfully Initialized!");

  //TODO: Find what the proper pin is to turn this LED
  // Drive SPI CS3 (IMU ready LED) low (active low)
  new DigitalOutput(28); 

  // Report usage and post data to DS
  HAL_Report(HALUsageReporting::kResourceType_ADIS16448, 0);
  SetName("ADIS16448", 0);
}

/**
  * @brief Switches to standard SPI operation. Primarily used when exiting auto SPI mode.
  *
  * @return A boolean indicating the success or failure of setting up the SPI peripheral in standard SPI mode.
  *
  * This function switches the active SPI port to standard SPI and is used primarily when 
  * exiting auto SPI. Exiting auto SPI is required to read or write using SPI since the 
  * auto SPI configuration, once active, locks the SPI message being transacted. This function
  * also verifies that the SPI port is operating in standard SPI mode by reading back the IMU
  * product ID. 
 **/
bool ADIS16448_IMU::SwitchToStandardSPI(){
  // Check to see whether the acquire thread is active. If so, wait for it to stop producing data.
  if (m_thread_active) {
    m_thread_active = false;
    while (!m_thread_idle) {
      Wait(0.01);
    }
    std::cout << "Paused the IMU processing thread successfully!" << std::endl;
    // Maybe we're in auto SPI mode? If so, kill auto SPI, and then SPI.
    if (m_spi != nullptr && m_auto_configured) {
      m_spi->StopAuto();
      // We need to get rid of all the garbage left in the auto SPI buffer after stopping it.
      // Sometimes data magically reappears, so we have to check the buffer size a couple of times
      //  to be sure we got it all. Yuck.
      uint32_t trashBuffer[200];
      Wait(0.1);
      int data_count = m_spi->ReadAutoReceivedData(trashBuffer, 0, 0_s);
      while (data_count > 0) {
        data_count = m_spi->ReadAutoReceivedData(trashBuffer, 0, 0_s);
        m_spi->ReadAutoReceivedData(trashBuffer, data_count, 0_s);
      }
      std::cout << "Paused the auto SPI successfully!" << std::endl;
      }
  }
  // There doesn't seem to be a SPI port active. Let's try to set one up
  if (m_spi == nullptr) {
    std::cout << "Setting up a new SPI port." << std::endl;
    m_spi = new SPI(m_spi_port);
    m_spi->SetClockRate(1000000);
    m_spi->SetMSBFirst();
    m_spi->SetSampleDataOnTrailingEdge();
    m_spi->SetClockActiveLow();
    m_spi->SetChipSelectActiveLow();
    ReadRegister(PROD_ID); // Dummy read

    // Validate the product ID
    uint16_t prod_id = ReadRegister(PROD_ID);
    if (prod_id != 16448) {
      DriverStation::ReportError("Could not find ADIS16448!");
      Close();
      return false;
    }
    return true;
  }
  else {
    // Maybe the SPI port is active, but not in auto SPI mode? Try to read the product ID.
    ReadRegister(PROD_ID); // Dummy read
    uint16_t prod_id = ReadRegister(PROD_ID);
    if (prod_id != 16448) {
      DriverStation::ReportError("Could not find ADIS16448!");
      Close();
      return false;
    }
    else {
      return true;
    }
  }
}

/**
  * This function switches the active SPI port to auto SPI and is used primarily when 
  * exiting standard SPI. Auto SPI is required to asynchronously read data over SPI as it utilizes
  * special FPGA hardware to react to an external data ready (GPIO) input. Data captured using auto SPI
  * is buffered in the FPGA and can be read by the CPU asynchronously. Standard SPI transactions are
  * impossible on the selected SPI port once auto SPI is enabled. The stall settings, GPIO interrupt pin,
  * and data packet settings used in this function are hard-coded to work only with the ADIS16470 IMU.
 **/
bool ADIS16448_IMU::SwitchToAutoSPI(){

  // No SPI port has been set up. Go set one up first.
  if(m_spi == nullptr){
    if(!SwitchToStandardSPI()){
      DriverStation::ReportError("Failed to start/restart auto SPI");
      return false;
    }
  }
  // Only set up the interrupt if needed.
  if (m_auto_interrupt == nullptr) {
    m_auto_interrupt = new DigitalInput(10);
  }
  // The auto SPI controller gets angry if you try to set up two instances on one bus.
  if (!m_auto_configured) {
    m_spi->InitAuto(8200);
    m_auto_configured = true;
  }
  // Set auto SPI packet data and size
  m_spi->SetAutoTransmitData(GLOB_CMD, 27);
  // Configure auto stall time  
  m_spi->ConfigureAutoStall(HAL_SPI_kMXP, 100, 1000, 255);
  // Kick off DMA SPI (Note: Device configration impossible after SPI DMA is activated)
  m_spi->StartAutoTrigger(*m_auto_interrupt, true, false);
  // Check to see if the acquire thread is running. If not, kick one off.
  if(!m_thread_idle) {
    m_first_run = true;
    m_thread_active = true;
    // Set up circular buffer
    std::vector<m_offset_data> m_offset_buffer(m_avg_size);
    std::vector<m_offset_data>::iterator m_offset_counter = m_offset_buffer.begin();
    // Kick off acquire thread
    m_acquire_task = std::thread(&ADIS16448_IMU::Acquire, this);
    std::cout << "New IMU Processing thread activated!" << std::endl;
  }
  else {
    m_first_run = true;
    m_thread_active = true;
    std::cout << "Old IMU Processing thread re-activated!" << std::endl;
  }
  // Looks like the thread didn't start for some reason. Abort.
  /*
  if(!m_thread_idle) {
    DriverStation::ReportError("Failed to start/restart the acquire() thread.");
    Close();
    return false;
  }
  */
  return true;
}

/**
 *
 */
int ADIS16448_IMU::ConfigCalTime(int new_cal_time) { 
  if(m_calibration_time == new_cal_time) {
    return 1;
  }
  else {
    m_calibration_time = (uint16_t)new_cal_time;
    m_avg_size = m_calibration_time * 819;
    return 0;
  }
}

//TODO: Fix this after I figure out how to implement the cal
void ADIS16448_IMU::Calibrate() {
  int buf_sumx = 0;
  int buf_sumy = 0;
  int buf_sumz = 0;
  for (std::vector<m_offset_data>::const_iterator n = m_offset_buffer.begin(); n != m_offset_buffer.end(); ++n)
  {
    buf_sumx += n->m_accum_gyro_x;
    buf_sumy += n->m_accum_gyro_y;
    buf_sumz += n->m_accum_gyro_z;
  }
  std::lock_guard<wpi::mutex> sync(m_mutex);
  m_gyro_offset_x = buf_sumx / (float)m_offset_buffer.size());
  m_gyro_offset_y = buf_sumy / (float)m_offset_buffer.size());
  m_gyro_offset_z = buf_sumz / (float)m_offset_buffer.size());
}

int ADIS16448_IMU::SetYawAxis(IMUAxis yaw_axis) {
  if (m_yaw_axis == yaw_axis) {
    return 1;
  }
  else {
    m_yaw_axis = yaw_axis;
    return 0;
  }
}

/**
  * This function reads the contents of an 8-bit register location by transmitting the register location
  * byte along with a null (0x00) byte using the standard WPILib API. The response (two bytes) is read 
  * back using the WPILib API and joined using a helper function. This function assumes the controller 
  * is set to standard SPI mode.
 **/
uint16_t ADIS16448_IMU::ReadRegister(uint8_t reg) {
  uint8_t buf[2];
  buf[0] = reg & 0x7f;
  buf[1] = 0;

  m_spi->Write(buf, 2);
  m_spi->Read(false, buf, 2);

  return ToUShort(buf);
}

/**
  * This function writes an unsigned, 16-bit value into adjacent 8-bit addresses via SPI. The upper
  * and lower bytes that make up the 16-bit value are split into two unsined, 8-bit values and written
  * to the upper and lower addresses of the specified register value. Only the lower (base) address
  * must be specified. This function assumes the controller is set to standard SPI mode.
 **/
void ADIS16448_IMU::WriteRegister(uint8_t reg, uint16_t val) {
  uint8_t buf[2];
  buf[0] = 0x80 | reg;
  buf[1] = val & 0xff;
  m_spi->Write(buf, 2);
  buf[0] = 0x81 | reg;
  buf[1] = val >> 8;
  m_spi->Write(buf, 2);
}

/**
  * This function resets (zeros) the accumulated (integrated) angle estimates for the xgyro, ygyro, and zgyro outputs.
 **/
void ADIS16448_IMU::Reset() {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  m_integ_gyro_x = 0.0;
  m_integ_gyro_y = 0.0;
  m_integ_gyro_z = 0.0;
}

void ADIS16448_IMU::Close() {
  if (m_thread_active) {
    m_thread_active = false;
    if (m_acquire_task.joinable()) m_acquire_task.join();
  }
  if (m_spi != nullptr) {
    if (m_auto_configured) {
      m_spi->StopAuto();
    }
  delete m_spi;
  m_auto_configured = false;
    if (m_auto_interrupt != nullptr) {
      delete m_auto_interrupt;
      m_auto_interrupt = nullptr;
    }
    m_spi = nullptr;
  }
  std::cout << "Finished cleaning up after the IMU driver." << std::endl;
}

ADIS16448_IMU::~ADIS16448_IMU() {
  Close();
}

void ADIS16448_IMU::Acquire() {
  // Set data packet length
  const int dataset_len = 29; // 18 data points + timestamp

  // This buffer can contain many datasets
  uint32_t buffer[4000];
  int data_count = 0;
  int data_remainder = 0;
  int data_to_read = 0;
  uint32_t previous_timestamp = 0;
  double delta_angle = 0.0;
  double gyro_x = 0.0;
  double gyro_y = 0.0;
  double gyro_z = 0.0;
  double accel_x = 0.0;
  double accel_y = 0.0;
  double accel_z = 0.0;
  double mag_x = 0.0;
  double mag_y = 0.0;
  double mag_z = 0.0;
  double baro = 0.0;
  double temp = 0.0;
  double gyro_x_si = 0.0;
  double gyro_y_si = 0.0;
  double gyro_z_si = 0.0;
  double accel_x_si = 0.0;
  double accel_y_si = 0.0;
  double accel_z_si = 0.0;
  double compAngleX = 0.0;
  double compAngleY = 0.0;
  double accelAngleX = 0.0;
  double accelAngleY = 0.0;

  while (true) {

	  // Sleep loop for 10ms (wait for data)
	  Wait(.01); 

    if (m_thread_active) {

      data_count = m_spi->ReadAutoReceivedData(buffer,0,0_s); // Read number of bytes currently stored in the buffer
      data_remainder = data_count % dataset_len; // Check if frame is incomplete
      data_to_read = data_count - data_remainder;  // Remove incomplete data from read count
      m_spi->ReadAutoReceivedData(buffer, data_to_read, 0_s); // Read data from DMA buffer

      // Could be multiple data sets in the buffer. Handle each one.
      for (int i = 0; i < data_to_read; i += dataset_len) { 
        // Timestamp is at buffer[i]
        m_dt = (buffer[i] - previous_timestamp) / 1000000.0;

        // Calculate CRC-16 on each data packet
        uint16_t calc_crc = 0xFFFF; // Starting word
        uint8_t byte = 0;
        uint16_t imu_crc = 0;
        for(int k = 5; k < 27; k += 2 ) // Cycle through XYZ GYRO, XYZ ACCEL, XYZ MAG, BARO, TEMP (Ignore Status & CRC)
        {
          byte = BuffToUByte(&buffer[i + k + 1]); // Process LSB
          calc_crc = (calc_crc >> 8) ^ adiscrc[(calc_crc & 0x00FF) ^ byte];
          byte = BuffToUByte(&buffer[i + k]); // Process MSB
          calc_crc = (calc_crc >> 8) ^ adiscrc[(calc_crc & 0x00FF) ^ byte];
        }
        calc_crc = ~calc_crc; // Complement
        calc_crc = (uint16_t)((calc_crc << 8) | (calc_crc >> 8)); // Flip LSB & MSB
        imu_crc = BuffToUShort(&buffer[i + 27]); // Extract DUT CRC from data buffer

        /*// Compare calculated vs read CRC. Don't update outputs or dt if CRC-16 is bad
        if (calc_crc == imu_crc) {*/

          gyro_x = BuffToShort(&buffer[i + 5]) * 0.04;
          gyro_y = BuffToShort(&buffer[i + 7]) * 0.04;
          gyro_z = BuffToShort(&buffer[i + 9]) * 0.04;
          accel_x = BuffToShort(&buffer[i + 11]) * 0.833;
          accel_y = BuffToShort(&buffer[i + 13]) * 0.833;
          accel_z = BuffToShort(&buffer[i + 15]) * 0.833;
          mag_x = BuffToShort(&buffer[i + 17]) * 0.1429;
          mag_y = BuffToShort(&buffer[i + 19]) * 0.1429;
          mag_z = BuffToShort(&buffer[i + 21]) * 0.1429;
          baro = BuffToShort(&buffer[i + 23]) * 0.02;
          temp = BuffToShort(&buffer[i + 25]) * 0.07386 + 31.0;

          // Convert scaled sensor data to SI units
          gyro_x_si = gyro_x * deg_to_rad;
          gyro_y_si = gyro_y * deg_to_rad;
          gyro_z_si = gyro_z * deg_to_rad;
          accel_x_si = accel_x * grav;
          accel_y_si = accel_y * grav;
          accel_z_si = accel_z * grav;

          // Store timestamp for next iteration
          previous_timestamp = buffer[i];

          m_alpha = m_tau / (m_tau + m_dt);

          if (m_first_run) {
            accelAngleX = atan2f(accel_x_si, sqrtf((accel_y_si * accel_y_si) + (accel_z_si * accel_z_si)));
            accelAngleY = atan2f(accel_y_si, sqrtf((accel_x_si * accel_x_si) + (accel_z_si * accel_z_si)));
            compAngleX = accelAngleX;
            compAngleY = accelAngleY;
          }
          else {
            // Process X angle
            accelAngleX = atan2f(accel_x_si, sqrtf((accel_y_si * accel_y_si) + (accel_z_si * accel_z_si)));
            accelAngleY = atan2f(accel_y_si, sqrtf((accel_x_si * accel_x_si) + (accel_z_si * accel_z_si)));
            accelAngleX = FormatAccelRange(accelAngleX, accel_z_si);
            accelAngleY = FormatAccelRange(accelAngleY, accel_z_si);
            compAngleX = CompFilterProcess(compAngleX, accelAngleX, -gyro_y_si);
            compAngleY = CompFilterProcess(compAngleY, accelAngleY, gyro_x_si);
          }

          // Update global state
          {
            std::lock_guard<wpi::mutex> sync(m_mutex);
            if (m_first_run) {
              m_integ_gyro_x = 0.0;
              m_integ_gyro_y = 0.0;
              m_integ_gyro_z = 0.0;
            }
            else {
              // Accumulate gyro for offset calibration
              ++m_offset_data::m_accum_count;
              if (m_offset_data::m_accum_count == m_offset_data::m_offset_buffer.end()) {
                m_offset_data::m_accum_count = m_offset_data::m_offset_buffer.begin();
              }
              m_offset_data::m_accum_gyro_x = gyro_x;
              m_offset_data::m_accum_gyro_y = gyro_y;
              m_offset_data::m_accum_gyro_z = gyro_z;

              // Accumulate gyro for angle integration
              m_integ_gyro_x += (gyro_x - m_gyro_offset_x) * m_dt;
              m_integ_gyro_y += (gyro_y - m_gyro_offset_y) * m_dt;
              m_integ_gyro_z += (gyro_z - m_gyro_offset_z) * m_dt;
            }
            m_gyro_x = gyro_x;
            m_gyro_y = gyro_y;
            m_gyro_z = gyro_z;
            m_accel_x = accel_x;
            m_accel_y = accel_y;
            m_accel_z = accel_z;
            m_mag_x = mag_x;
            m_mag_y = mag_y;
            m_mag_z = mag_z;
            m_baro = baro;
            m_temp = temp;
            m_compAngleX = compAngleX * rad_to_deg;
            m_compAngleY = compAngleY * rad_to_deg;
            m_accelAngleX = accelAngleX * rad_to_deg;
            m_accelAngleY = accelAngleY * rad_to_deg;
          }
          m_first_run = false;
        /*}
        else {
          // Print notification when crc fails and bad data is rejected
          std::cout << "IMU Data CRC Mismatch Detected." << std::endl;
          std::cout << "Calculated CRC: " << calc_crc << std::endl;
          std::cout << "Read CRC: " << imu_crc << std::endl;
          // DEBUG: Plot sub-array data in terminal
          std::cout << BuffToUShort(&buffer[i + 3]) << "," << BuffToUShort(&buffer[i + 5]) << "," << BuffToUShort(&buffer[i + 7]) <<
          "," << BuffToUShort(&buffer[i + 9]) << "," << BuffToUShort(&buffer[i + 11]) << "," << BuffToUShort(&buffer[i + 13]) << "," <<
          BuffToUShort(&buffer[i + 15]) << "," << BuffToUShort(&buffer[i + 17]) << "," << BuffToUShort(&buffer[i + 19]) << "," <<
          BuffToUShort(&buffer[i + 21]) << "," << BuffToUShort(&buffer[i + 23]) << "," << BuffToUShort(&buffer[i + 25]) << "," <<
          BuffToUShort(&buffer[i + 27]) << std::endl; 
        }*/
      }
    }
    else {
      m_thread_idle = true;
      data_count = 0;
      data_remainder = 0;
      data_to_read = 0;
      previous_timestamp = 0.0;
      delta_angle = 0.0;
      gyro_x = 0.0;
      gyro_y = 0.0;
      gyro_z = 0.0;
      accel_x = 0.0;
      accel_y = 0.0;
      accel_z = 0.0;
      mag_x = 0.0;
      mag_y = 0.0;
      mag_z = 0.0;
      baro = 0.0;
      temp = 0.0;
      gyro_x_si = 0.0;
      gyro_y_si = 0.0;
      gyro_z_si = 0.0;
      accel_x_si = 0.0;
      accel_y_si = 0.0;
      accel_z_si = 0.0;
      compAngleX = 0.0;
      compAngleY = 0.0;
      accelAngleX = 0.0;
      accelAngleY = 0.0;
    }
  }
}

/* Complementary filter functions */
double ADIS16448_IMU::FormatFastConverge(double compAngle, double accAngle) {
  if(compAngle > accAngle + M_PI) {
    compAngle = compAngle - 2.0 * M_PI;
  }
  else if (accAngle > compAngle + M_PI) {
    compAngle = compAngle + 2.0 * M_PI;
  }
  return compAngle;  
}

double ADIS16448_IMU::FormatRange0to2PI(double compAngle) {
  while(compAngle >= 2 * M_PI) {
    compAngle = compAngle - 2.0 * M_PI;
  }
  while(compAngle < 0.0) {
    compAngle = compAngle + 2.0 * M_PI;
  }
  return compAngle;
}

double ADIS16448_IMU::FormatAccelRange(double accelAngle, double accelZ) {
  if(accelZ < 0.0) {
    accelAngle = M_PI - accelAngle;
  }
  else if(accelZ > 0.0 && accelAngle < 0.0) {
    accelAngle = 2.0 * M_PI + accelAngle;
  }
  return accelAngle;
}

double ADIS16448_IMU::CompFilterProcess(double compAngle, double accelAngle, double omega) {
  compAngle = FormatFastConverge(compAngle, accelAngle);
  compAngle = m_alpha * (compAngle + omega * m_dt) + (1.0 - m_alpha) * accelAngle;
  compAngle = FormatRange0to2PI(compAngle);
  if(compAngle > M_PI) {
    compAngle = compAngle - 2.0 * M_PI;
  }
  return compAngle;
}

/**
  * This function returns the most recent integrated angle for the axis chosen by m_yaw_axis. 
  * This function is most useful in situations where the yaw axis may not coincide with the IMU
  * Z axis. 
 **/
double ADIS16448_IMU::GetAngle() const {
  switch (m_yaw_axis) {
    case kX:
      return GetGyroAngleX();
    case kY:
      return GetGyroAngleY();
    case kZ:
      return GetGyroAngleZ();
    default:
      return 0.0;
  }
}

double ADIS16448_IMU::GetRate() const {
  switch (m_yaw_axis) {
    case kX:
      return GetGyroInstantX();
    case kY:
      return GetGyroInstantY();
    case kZ:
      return GetGyroInstantZ();
    default:
      return 0.0;
  }
}

ADIS16448_IMU::IMUAxis ADIS16448_IMU::GetYawAxis() const {
  return m_yaw_axis;
}

double ADIS16448_IMU::GetGyroAngleX() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_integ_gyro_x;
}

double ADIS16448_IMU::GetGyroAngleY() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_integ_gyro_y;
}

double ADIS16448_IMU::GetGyroAngleZ() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_integ_gyro_z;
}

double ADIS16448_IMU::GetGyroInstantX() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_gyro_x;
}

double ADIS16448_IMU::GetGyroInstantY() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_gyro_y;
}

double ADIS16448_IMU::GetGyroInstantZ() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_gyro_z;
}

double ADIS16448_IMU::GetAccelInstantX() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_accel_x;
}

double ADIS16448_IMU::GetAccelInstantY() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_accel_y;
}

double ADIS16448_IMU::GetAccelInstantZ() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_accel_z;
}

double ADIS16448_IMU::GetMagInstantX() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_mag_x;
}

double ADIS16448_IMU::GetMagInstantY() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_mag_y;
}

double ADIS16448_IMU::GetMagInstantZ() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_mag_z;
}

double ADIS16448_IMU::GetXComplementaryAngle() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_compAngleX;
}

double ADIS16448_IMU::GetYComplementaryAngle() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_compAngleY;
}

double ADIS16448_IMU::GetXFilteredAccelAngle() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_accelAngleX;
}

double ADIS16448_IMU::GetYFilteredAccelAngle() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_accelAngleY;
}

double ADIS16448_IMU::GetBarometricPressure() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_baro;
}

double ADIS16448_IMU::GetTemperature() const {
  std::lock_guard<wpi::mutex> sync(m_mutex);
  return m_temp;
}

void ADIS16448_IMU::InitSendable(SendableBuilder& builder) {
  builder.SetSmartDashboardType("ADIS16448 IMU");
  auto gyroX = builder.GetEntry("GyroX").GetHandle();
  auto gyroY = builder.GetEntry("GyroY").GetHandle();
  auto gyroZ = builder.GetEntry("GyroZ").GetHandle();
  auto accelX = builder.GetEntry("AccelX").GetHandle();
  auto accelY = builder.GetEntry("AccelY").GetHandle();
  auto accelZ = builder.GetEntry("AccelZ").GetHandle();
  auto angleX = builder.GetEntry("AngleX").GetHandle();
  auto angleY = builder.GetEntry("AngleY").GetHandle();
  auto angleZ = builder.GetEntry("AngleZ").GetHandle();
  builder.SetUpdateTable([=]() {
	nt::NetworkTableEntry(gyroX).SetDouble(GetGyroInstantX());
	nt::NetworkTableEntry(gyroY).SetDouble(GetGyroInstantY());
	nt::NetworkTableEntry(gyroZ).SetDouble(GetGyroInstantZ());
	nt::NetworkTableEntry(accelX).SetDouble(GetAccelInstantX());
	nt::NetworkTableEntry(accelY).SetDouble(GetAccelInstantY());
	nt::NetworkTableEntry(accelZ).SetDouble(GetAccelInstantZ());
	nt::NetworkTableEntry(angleX).SetDouble(GetGyroAngleX());
	nt::NetworkTableEntry(angleY).SetDouble(GetGyroAngleY());
	nt::NetworkTableEntry(angleZ).SetDouble(GetGyroAngleZ());
  });
}
