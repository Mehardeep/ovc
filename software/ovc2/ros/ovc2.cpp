#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>  // ioctl
#include <sys/mman.h>   // mmap
#include <unistd.h>

#include "ovc2.h"
#include "../modules/ovc2_core/ovc2_ioctl.h"

using ovc2::OVC2;

static const char * const OVC2_DEVICE = "/dev/ovc2_core";
static const char * const OVC2_IMU_DEVICE = "/dev/ovc2_imu";

OVC2::OVC2()
: init_complete_(false),
  fd_(-1),
  fd_imu_(-1),
  imu_serial(NULL)
{
}

OVC2::~OVC2()
{
  if (init_complete_) {
    close(fd_);
    close(fd_imu_);
  }
  if (imu_serial) {
    delete imu_serial;
    imu_serial = NULL;
  }
}

bool OVC2::init()
{
  fd_ = open(OVC2_DEVICE, O_RDWR);
  if (fd_ < 0) {
    printf("couldn't open %s\n", OVC2_DEVICE);
    return false;
  }
  fd_imu_ = open(OVC2_IMU_DEVICE, O_RDONLY);
  if (fd_imu_ < 0) {
    printf("couldn't open %s\n", OVC2_IMU_DEVICE);
    return false;
  }
  if (!enable_reg_ram())
    return false;
  printf("reg ram init complete\n");
  imu_serial = new LightweightSerial("/dev/ttyTHS2", 115200);
  if (!imu_serial->is_ok()) {
    printf("OH NO couldn't open IMU serial port\n");
    return false;
  }
  init_complete_ = true;
  return true;
}

bool OVC2::enable_reg_ram()
{
  struct ovc2_ioctl_enable_reg_ram e;
  e.enable = 1;
  int rc = ioctl(fd_, OVC2_IOCTL_ENABLE_REG_RAM, &e);
  if (rc != 0)
    printf("uh oh: enable_reg_ram ioctl rc = %d\n", rc);
  return (rc == 0);
}

bool OVC2::set_bit(const int reg_idx, const int bit_idx, const bool bit_value)
{
  struct ovc2_ioctl_set_bit sb;
  sb.reg_idx = reg_idx;
  sb.bit_idx = bit_idx;
  sb.state = bit_value ? 1 : 0;
  int rc = ioctl(fd_, OVC2_IOCTL_SET_BIT, &sb);
  if (rc != 0)
    printf("OVC2::set_bit() ioctl rc = %d\n", rc);
  return (rc == 0);
}

int OVC2::spi_read(const int bus, const int reg)
{
  struct ovc2_ioctl_spi_xfer spi_xfer;
  spi_xfer.dir = OVC2_IOCTL_SPI_XFER_DIR_READ;
  spi_xfer.bus = bus;
  spi_xfer.reg_addr = reg;
  spi_xfer.reg_val = 0;
  int rc = ioctl(fd_, OVC2_IOCTL_SPI_XFER, &spi_xfer);
  if (rc != 0) {
    printf("ovc2 spi_read() ioctl rc = %d\n", rc);
    return rc;
  }
  return spi_xfer.reg_val;
}

bool OVC2::reset_imagers()
{
  if (!set_bit(0, 29, true))  // assert imager resets
    return false;
  usleep(10000);  // wait a bit
  if (!set_bit(0, 29, false))  // de-assert imager resets
    return false;
  usleep(50000);  // wait for imagers to wake back up
  if (!set_bit(0, 28, true))  // turn on camera clock
    return false;
  return true;
}

bool OVC2::configure_imagers()
{
  if (!reset_imagers()) {
    printf("OVC2::configure_imagers(): reset_imagers() failed\n");
    return false;
  }
  // requires rework on ovc2a to talk to imager #2...
  for (int i = 0; i < 2; i++) {
		if (!configure_imager(i)) {
      printf("OH NO couldn't configure imager %d\n", i);
      return false;
    }
    if (!align_imager_lvds(i)) {
      printf("OH NO couldn't align LVSD stream of imager %d\n", i);
      return false;
    }
    printf("imager %d configured successfully\n", i);
  }
  return true;
}

// save some typing...
#define ADD_REG(idx,val) regs.push_back(ImagerRegister(idx, val))

bool OVC2::configure_imager(const int imager_idx)
{
  if (imager_idx != 0 && imager_idx != 1)
    return false;
  std::vector<ImagerRegister> regs;

  ADD_REG(32, 0x4008);  // enable logic clock
  ADD_REG(20, 0x0001);  // enable LVDS clock input
  ADD_REG( 9, 0x0000);  // release clkgen soft-reset
  ADD_REG(32, 0x400a);  // enable logic clock
  ADD_REG(34, 0x0001);  // enable logic blocks

  // magic register settings... don't ask questions.
  ADD_REG( 41, 0x08aa);
  ADD_REG( 42, 0x4110);
  ADD_REG( 43, 0x0008);
  ADD_REG( 65, 0x382b);
  ADD_REG( 66, 0x53c8);  // bias current
  ADD_REG( 67, 0x0665);
  ADD_REG( 68, 0x0088);  // lvds comm+diff level
  ADD_REG( 70, 0x1111);
  ADD_REG( 72, 0x0017);
  ADD_REG(128, 0x4714);
  ADD_REG(129, 0xa001);  // 8-bit mode
  ADD_REG(171, 0x1002);
  ADD_REG(175, 0x0080);
  ADD_REG(176, 0x00e6);
  ADD_REG(177, 0x0400);

  //ADD_REG(192, 0x100c);  // sequencer general config: master mode
  ADD_REG(192, 0x103c);  // sequencer general config: slave mode
  ADD_REG(194, 0x0224);  // integration_control
  ADD_REG(197, 0x0306);  // black_lines
  ADD_REG(204, 0x01e1);  // set gain to 1.0
  ADD_REG(207, 0x0000);  // ref_lines
  ADD_REG(211, 0x0e49);
  ADD_REG(215, 0x111f);
  ADD_REG(216, 0x7f00);
  ADD_REG(219, 0x0020);
  ADD_REG(220, 0x3a28);
  ADD_REG(221, 0x624d);
  ADD_REG(222, 0x624d);
  ADD_REG(224, 0x3e5e);
  ADD_REG(227, 0x0000);
  ADD_REG(250, 0x2081);
  ADD_REG(384, 0xc800);
  ADD_REG(384, 0xC800);
  ADD_REG(385, 0xFB1F);
  ADD_REG(386, 0xFB1F);
  ADD_REG(387, 0xFB12);
  ADD_REG(388, 0xF903);
  ADD_REG(389, 0xF802);
  ADD_REG(390, 0xF30F);
  ADD_REG(391, 0xF30F);
  ADD_REG(392, 0xF30F);
  ADD_REG(393, 0xF30A);
  ADD_REG(394, 0xF101);
  ADD_REG(395, 0xF00A);
  ADD_REG(396, 0xF24B);
  ADD_REG(397, 0xF226);
  ADD_REG(398, 0xF001);
  ADD_REG(399, 0xF402);
  ADD_REG(400, 0xF001);
  ADD_REG(401, 0xF402);
  ADD_REG(402, 0xF001);
  ADD_REG(403, 0xF401);
  ADD_REG(404, 0xF007);
  ADD_REG(405, 0xF20F);
  ADD_REG(406, 0xF20F);
  ADD_REG(407, 0xF202);
  ADD_REG(408, 0xF006);
  ADD_REG(409, 0xEC02);
  ADD_REG(410, 0xE801);
  ADD_REG(411, 0xEC02);
  ADD_REG(412, 0xE801);
  ADD_REG(413, 0xEC02);
  ADD_REG(414, 0xC801);
  ADD_REG(415, 0xC800);
  ADD_REG(416, 0xC800);
  ADD_REG(417, 0xCC02);
  ADD_REG(418, 0xC801);
  ADD_REG(419, 0xCC02);
  ADD_REG(420, 0xC801);
  ADD_REG(421, 0xCC02);
  ADD_REG(422, 0xC805);
  ADD_REG(423, 0xC800);
  ADD_REG(424, 0x0030);
  ADD_REG(425, 0x207C);
  ADD_REG(426, 0x2071);
  ADD_REG(427, 0x0074);
  ADD_REG(428, 0x107F);
  ADD_REG(429, 0x1072);
  ADD_REG(430, 0x1074);
  ADD_REG(431, 0x0076);
  ADD_REG(432, 0x0031);
  ADD_REG(433, 0x21BB);
  ADD_REG(434, 0x20B1);
  ADD_REG(435, 0x20B1);
  ADD_REG(436, 0x00B1);
  ADD_REG(437, 0x10BF);
  ADD_REG(438, 0x10B2);
  ADD_REG(439, 0x10B4);
  ADD_REG(440, 0x00B1);
  ADD_REG(441, 0x0030);
  ADD_REG(442, 0x0030);
  ADD_REG(443, 0x217B);
  ADD_REG(444, 0x2071);
  ADD_REG(445, 0x2071);
  ADD_REG(446, 0x0074);
  ADD_REG(447, 0x107F);
  ADD_REG(448, 0x1072);
  ADD_REG(449, 0x1074);
  ADD_REG(450, 0x0076);
  ADD_REG(451, 0x0031);
  ADD_REG(452, 0x20BB);
  ADD_REG(453, 0x20B1);
  ADD_REG(454, 0x20B1);
  ADD_REG(455, 0x00B1);
  ADD_REG(456, 0x10BF);
  ADD_REG(457, 0x10B2);
  ADD_REG(458, 0x10B4);
  ADD_REG(459, 0x00B1);
  ADD_REG(460, 0x0030);
  ADD_REG(461, 0x0030);
  ADD_REG(462, 0x207C);
  ADD_REG(463, 0x2071);
  ADD_REG(464, 0x0073);
  ADD_REG(465, 0x017A);
  ADD_REG(466, 0x0078);
  ADD_REG(467, 0x1074);
  ADD_REG(468, 0x0076);
  ADD_REG(469, 0x0031);
  ADD_REG(470, 0x21BB);
  ADD_REG(471, 0x20B1);
  ADD_REG(472, 0x20B1);
  ADD_REG(473, 0x00B1);
  ADD_REG(474, 0x10BF);
  ADD_REG(475, 0x10B2);
  ADD_REG(476, 0x10B4);
  ADD_REG(477, 0x00B1);
  ADD_REG(478, 0x0030);
  ADD_REG(206, 0x077f);

	// soft power up
  ADD_REG(32 , 0x400b);  // enable analog clock
  ADD_REG(10 , 0x0000);  // release soft reset
  ADD_REG(64 , 0x0001);  // enable biasing block
  ADD_REG(72 , 0x0017);  // enable charge pump
  ADD_REG(40 , 0x0003);  // enable col. multiplexer
  ADD_REG(42 , 0x4113);  // configure image core
  ADD_REG(48 , 0x0001);  // enable analog front-end
  ADD_REG(112, 0x0007);  // enable LVDS transmitters

  // set exposure to 5 ms
  // NOTE: this is not used anymore with external trigger (slave mode) enabled,
  // which is driven by the FPGA timing
	ADD_REG(199, 32);  // set mult_timer to 128
  int ticks = (int)(0.005 / (128.0 / 250.0e6));  // 9765 for 5ms
  ADD_REG(201, (uint16_t)ticks);

  ADD_REG(192, 0x103d);  // enable sequencer in slave mode

  // now blast through and actually set all those registers
  for (auto &r: regs)
    if (!write_imager_reg(imager_idx, r))
      return false;

  printf("wrote all registers successfully to imager %d\n", imager_idx);

  return true;
}

bool OVC2::read_imager_reg(const int imager_idx, ImagerRegister &reg)
{
  if (imager_idx != 0 && imager_idx != 1)
    return false;  // get outta here
  struct ovc2_ioctl_spi_xfer spi_xfer;
  spi_xfer.dir = OVC2_IOCTL_SPI_XFER_DIR_READ;
  spi_xfer.bus = imager_idx;
  spi_xfer.reg_addr = reg.index;
  spi_xfer.reg_val = 0;
  int rc = ioctl(fd_, OVC2_IOCTL_SPI_XFER, &spi_xfer);
  if (rc != 0) {
    printf("oh no! error reading imager %d register %d\n",
      (int)imager_idx, (int)reg.index);
    return false;
  }
  reg.value = spi_xfer.reg_val;
  return true;
}

bool OVC2::write_imager_reg(const int imager_idx, const ImagerRegister reg)
{
  if (imager_idx != 0 && imager_idx != 1)
    return false;
  struct ovc2_ioctl_spi_xfer spi_xfer;
  spi_xfer.dir = OVC2_IOCTL_SPI_XFER_DIR_WRITE;
  spi_xfer.bus = imager_idx;
  spi_xfer.reg_addr = reg.index;
  spi_xfer.reg_val = reg.value;
  int rc = ioctl(fd_, OVC2_IOCTL_SPI_XFER, &spi_xfer);
  if (rc != 0) {
    printf("oh no! error setting spi register %d to 0x%04x\n",
      (int)reg.index, (unsigned)reg.value);
    return false;
  }

  return true;
}

bool OVC2::align_imager_lvds(const int imager_idx)
{
  if (imager_idx != 0 && imager_idx != 1)
    return false;
  struct ovc2_ioctl_read_pio rp;
  rp.channel = imager_idx << 2;
  uint8_t sync_data = 0;
  bool sync_ok = false;
  int bitslips = 0;
  int rc = 0;
  for (int attempt = 0; attempt < 1000; attempt++) {
    rc = ioctl(fd_, OVC2_IOCTL_READ_PIO, &rp);
    if (rc) {
      printf("OH NO rc from kernel module when reading PIO data: %d\n", rc);
      return false;
    }
    sync_data = (uint8_t)(rp.data >> 24);
    printf("imager %d sync lane : 0x%02x\n", imager_idx, sync_data);

    if (sync_data == 0x00)
      continue;  // this word isn't informative...
    if (sync_data == 0x0d || sync_data == 0x0a || sync_data == 0xe9 ||
        sync_data == 0xaa || sync_data == 0xca || sync_data == 0x4a ||
        sync_data == 0x16 || sync_data == 0x05) {
      sync_ok = true;
      break;
    }
    if (!sync_ok) {
      // if we get here, we need to slip the sync channel
      struct ovc2_ioctl_bitslip bs;
      bs.channels = (imager_idx == 0 ? 0x10 : 0x200);
      bitslips++;
      printf("  bitslip(0x%02x)\n", (unsigned)bs.channels);
      rc = ioctl(fd_, OVC2_IOCTL_BITSLIP, &bs);
      if (rc) {
        printf("OH NO weird rc from bitslip ioctl: %d\n", rc);
        return false;
      }
    }
    usleep(10);  // wait for bitslip request to propagate through chain
  }
  if (!sync_ok) {
    printf("OH NO, couldn't align sync channel on imager %d\n", imager_idx);
    return false;
  }
  if (bitslips)
    printf("  sync channel aligned OK after %d rotations\n", bitslips);

  for (int channel_idx = 0; channel_idx < 4; channel_idx++) {
    rp.channel = channel_idx + imager_idx * 4;
    bool channel_ok = false;
    bitslips = 0;
    for (int attempt = 0; attempt < 1000; attempt++) {
      int rc = ioctl(fd_, OVC2_IOCTL_READ_PIO, &rp);
      if (rc) {
        printf("OH NO rc from kernel module when reading PIO data: %d\n", rc);
        return false;
      }
      uint8_t channel_data = (uint8_t)(rp.data >> 16);
      printf("imager %d channel %d: 0x%02x\n",
          imager_idx, channel_idx, channel_data);
      if (channel_data == 0xe9) {
        channel_ok = true;
        break;
      }
      // channel data wasn't the training word 0xe9. time to rotate.
      struct ovc2_ioctl_bitslip bs;
      bs.channels = 1 << (channel_idx + imager_idx * 5);
      printf("  bitslip(0x%02x)\n", (unsigned)bs.channels);
      bitslips++;
      rc = ioctl(fd_, OVC2_IOCTL_BITSLIP, &bs);
      if (rc) {
        printf("OH NO weird rc from bitslip ioctl: %d\n", rc);
        return false;
      }
      usleep(10);  // wait for bitslip request to propagate through chain
    }
    if (!channel_ok) {
      printf("OH NO couldn't align data lane %d on imager %d\n",
        channel_idx, imager_idx);
      return false;
    }
    if (bitslips)
      printf("  lane %d aligned after %d rotations\n", channel_idx, bitslips);
  }
  printf("all LVDS links on imager %d aligned successfully\n", imager_idx);
  return true;
}

bool OVC2::configure_imu()
{
  printf("OVC2::configure_imu()\n");
  imu_set_auto_poll(false);
  // first, reset it...
  if (!set_bit(0, 27, true))  // assert imu reset pin
    return false;
  usleep(1000);
  if (!set_bit(0, 27, false))  // de-assert imu reset pin
    return false;
  printf("waiting 2 seconds for IMU reset...\n");
  usleep(2000000);
  write_imu_reg_str("$VNWRG,06,0");  // turn off async data output
  // now drain all the async output that was stuffed in our buffers
  for (int i = 0; i < 100; i++) {
    printf("waiting for rx...\n");
    uint8_t line[256];
    int nread = imu_serial->read_block(line, sizeof(line));
    printf("read %d bytes\n", nread);
    if (!nread)
      break;
  }
  // need to write a long config string for the sync pulse now
  // request configuration: ignore SYNC_IN and drive SYNC_OUT pulses
  // every 2nd AHRS measurement (= 200 Hz) of 100us width
  write_imu_reg_str("$VNWRG,32,3,0,0,0,3,1,1,100000,0");
  imu_set_auto_poll(true);
}

bool OVC2::write_imu_reg_str(const char * const reg_str)
{
  char request[256] = {0};
  strncpy(request, reg_str, sizeof(request)-10);
  imu_append_checksum(request);
  printf("sending IMU request: [%s]\n", request);
  imu_serial->write_cstr(request);
  uint8_t response[256] = {0};
  bool response_ok = false;
  // wait for response
  for (int attempt = 0; attempt < 100; attempt++) {
    int nread = imu_serial->read_block(response, sizeof(response));
    if (nread == 0) {
      usleep(1000);
      continue;
    }
    for (int i = 0; i < nread; i++) {
      if (response[i] == '\n') {
        //printf("received newline.\n");
        response_ok = true;  // todo: actually look at the response
      }
    }
    if (response_ok)
      break;
  }
  return response_ok;
}

bool OVC2::imu_append_checksum(char *msg)
{
  if (!msg) {
    printf("WOAH THERE PARTNER. you send append_checksum() a null string.\n");
    return false;
  }
  if (msg[0] != '$') {
    printf("WOAH THERE PARTNER. expected message string to begin with '$'\n");
    return false;
  }
  int msg_len = strlen(msg);
  uint8_t csum = 0; 
  for (int i = 1; i < msg_len; i++)
    csum ^= msg[i];
  char csum_ascii[10] = {0};
  snprintf(csum_ascii, sizeof(csum_ascii), "*%02x\r\n", (int)csum);
  strcat(msg, csum_ascii);
  return true;
}

bool OVC2::imu_set_auto_poll(bool enable)
{
  struct ovc2_ioctl_imu_set_mode ism;
  if (enable)
    ism.mode = OVC2_IOCTL_IMU_SET_MODE_AUTO;
  else
    ism.mode = OVC2_IOCTL_IMU_SET_MODE_IDLE;
  int rc = ioctl(fd_, OVC2_IOCTL_IMU_SET_MODE, &ism);
  if (rc) {
    printf("OH NO couldn't set IMU autopoll mode\n");
    return false;
  }
  return true;
}

bool OVC2::wait_for_imu_data(bool print_to_console)
{
  //struct ovc2_imu_data imu_data;
  int nread = read(fd_imu_, &imu_data_, sizeof(imu_data_));
  if (nread != sizeof(imu_data_)) {
    printf("got weird nread: %d\n", nread);
    return false;
  }
  struct ovc2_imu_data *i = &imu_data_;  // save typing
  if (print_to_console) {
    printf("read %d bytes from IMU\n", nread);
    printf("\n\n");
    printf("t_usecs = %llu\n", (long long unsigned)i->t_usecs);
    printf("accel = %+.3f  %+.3f  %+.3f\n",
      i->accel[0], i->accel[1], i->accel[2]);
    printf("gyro  = %+.3f  %+.3f  %+.3f\n",
      i->gyro[0], i->gyro[1], i->gyro[2]);
    printf("temp  = %+.3f\n", i->temperature);
    printf("pressure = %+.3f\n", i->pressure);
    printf("quat = %+.3f  %+.3f  %+.3f  %+.3f\n",
      i->quaternion[0], i->quaternion[1],
      i->quaternion[2], i->quaternion[3]);
    printf("mag = %+.3f  %+.3f  %+.3f\n",
      i->mag_comp[0], i->mag_comp[1], i->mag_comp[2]);
  }
  return true;
}
