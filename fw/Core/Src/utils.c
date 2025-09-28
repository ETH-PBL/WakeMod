#include "utils.h"
#include "fh101rf.h"
#include "fh101rf_reg.h"
#include "main.h"
#include "ook.h"
#include "stm32_hal_legacy.h"
#include "stm32u0xx.h"
#include "stm32u0xx_hal.h"
#include "stm32u0xx_hal_def.h"
#include "stm32u0xx_hal_gpio.h"
#include "stm32u0xx_hal_i2c_ex.h"
#include "stm32u0xx_hal_pwr.h"
#include "stm32u0xx_hal_pwr_ex.h"
#include "ulog.h"
#include <string.h>

// Private Variables
volatile uint8_t wur_irq_flag = false;
volatile uint8_t wur_spi_rx_cplt_flag = false;
volatile uint8_t wur_spi_tx_cplt_flag = false;
SPI_HandleTypeDef *wur_spi = &hspi2;

volatile uint8_t ook_spi_tx_cplt_flag = false;
SPI_HandleTypeDef *ook_spi = &hspi1;

// Driver Helper Functions
fh101rf_err_t init_struct_fh101rf(struct fh101rf_h *dev) {
  // Load default configuration
  fh101rf_err_t err = fh101rf_init_struct_with_reset_val(&dev->conf);

  // Adjust default configurations
  {
    dev->conf.nfa868_slow.data = FH101RF_SAMPLE_RATE_SR_1_024;
    dev->conf.nfa868_fast.data = FH101RF_SAMPLE_RATE_SR_32_768;

    dev->conf.n_lco_target_868_lo.data =
        (uint8_t)(3465 & 0xFF); // (/ (* (+ 868.35 40) 1000000) (* 8 32768)) //
                                // 40MHz above carrier
    dev->conf.n_lco_target_868_hi.data =
        (uint8_t)((3465 >> 8) & 0xFF); // (/ (* (+ 868.35 40) 1000000) (* 8
                                       // 32768)) // 40MHz above carrier
    dev->conf.band_branch_ctrl.active_bands.band_433 = false;
    dev->conf.band_branch_ctrl.active_bands.band_868 = true;
    dev->conf.band_branch_ctrl.active_bands.band_2g4 = false;
    dev->conf.band_branch_ctrl.active_branches.weak = true;
    dev->conf.band_branch_ctrl.active_branches.medium = true;
    dev->conf.band_branch_ctrl.active_branches.strong = true;

    dev->conf.code_select.a = FH101RF_BIN_CODE_A;
    dev->conf.code_select.b = FH101RF_BIN_CODE_B;

    dev->conf.korrel_thresh_a.data = 0x1A;
    dev->conf.korrel_thresh_b.data = 0x1A;

    dev->conf.fdd_enable.fdd_bands.band_433 = false;
    dev->conf.fdd_enable.fdd_bands.band_868 = true;
    dev->conf.fdd_enable.fdd_bands.band_2g4 = false;

    dev->conf.irq_select.irq_select.correl_match = false;
    dev->conf.irq_select.irq_select.id_match = true;
    dev->conf.irq_select.irq_select.id_match_and_ldr = false;

    dev->conf.idm_enable.match_bands.band_433 = false;
    dev->conf.idm_enable.match_bands.band_868 = true;
    dev->conf.idm_enable.match_bands.band_2g4 = false;

    dev->conf.idm_ctrl.ctrl = FH101RF_CTRL_IND_GROUP_BROAD;

    dev->conf.fifo_length.band_433 = FH101RF_FIFO_LEN_BIT16;
    dev->conf.fifo_length.band_868 = FH101RF_FIFO_LEN_BIT32;
    dev->conf.fifo_length.band_2g4 = FH101RF_FIFO_LEN_BIT16;

    dev->conf.xtal_osc_ctrl.data = 0x01;
    dev->conf.ldo_xtal_ctrl.xtal_osc_byp = false;
    dev->conf.ldo_xtal_ctrl.ldo_ena_nfa = false;

    dev->conf.mux_d_out_sel.out = FH101RF_OUT_IRQ_EVENT_1;

    return err;
  }
}

void driver_log(char *msg, bool is_err, bool has_int_arg, uint32_t arg) {
#ifdef DEBUG
  if (is_err) {
    has_int_arg ? ULOG_ERR("%s, with value: %d", msg, arg)
                : ULOG_ERR("%s", msg);
  } else {
    has_int_arg ? ULOG_INF("%s, with value: %d", msg, arg)
                : ULOG_ERR("%s", msg);
  }
#endif
}

void set_ant_control(enum ant_ctrl dev) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = ANT_CTRL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  if (dev == FH101RF_DEV) {
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(ANT_CTRL_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(ANT_CTRL_GPIO_Port, ANT_CTRL_Pin, GPIO_PIN_RESET);
    HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_B, ANT_CTRL_Pin);

  } else if (dev == OOK_DEV) {
    HAL_PWREx_DisableGPIOPullDown(PWR_GPIO_B, ANT_CTRL_Pin);
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(ANT_CTRL_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(ANT_CTRL_GPIO_Port, ANT_CTRL_Pin, GPIO_PIN_SET);
  }
}

fh101rf_err_t fh101rf_spi_write_reg(uint8_t reg_adr, uint8_t val) {
  if (wur_spi == NULL) {
    return E_FH101RF_NULLPTR_ERR;
  }

  uint8_t buf[2] = {reg_adr, val};
  wur_spi_tx_cplt_flag = false;
  HAL_GPIO_WritePin(WUR_CS_GPIO_Port, WUR_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef ret = HAL_SPI_Transmit_DMA(wur_spi, buf, 2);
  while (!wur_spi_tx_cplt_flag) {
    goto_sleep();
  }
  HAL_GPIO_WritePin(WUR_CS_GPIO_Port, WUR_CS_Pin, GPIO_PIN_SET);

  if (ret == HAL_OK) {
    return E_FH101RF_SUCCESS;
  }

  return E_FH101RF_COM_ERR;
}

fh101rf_err_t fh101rf_spi_read_reg(uint8_t reg_adr, uint8_t *res) {
  if (wur_spi == NULL) {
    return E_FH101RF_NULLPTR_ERR;
  }

  uint8_t buf[2] = {reg_adr, 0x00};
  uint8_t result[2] = {0x00, 0x00};
  wur_spi_tx_cplt_flag = false;
  HAL_GPIO_WritePin(WUR_CS_GPIO_Port, WUR_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef ret = HAL_SPI_Transmit_DMA(wur_spi, buf, 1);
  while (!wur_spi_tx_cplt_flag) {
    goto_sleep();
  }

  wur_spi_rx_cplt_flag = false;
  ret = HAL_SPI_Receive_DMA(wur_spi, result, 1);
  while (!wur_spi_rx_cplt_flag) {
    goto_sleep();
  }

  HAL_GPIO_WritePin(WUR_CS_GPIO_Port, WUR_CS_Pin, GPIO_PIN_SET);

  if (ret == HAL_OK) {
    *res = result[0];
    return E_FH101RF_SUCCESS;
  }

  return E_FH101RF_COM_ERR;
}

void fh101rf_rst_set(bool level) {
  HAL_GPIO_WritePin(WUR_RST_GPIO_Port, WUR_RST_Pin,
                    level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

ook_err_t ook_spi_write(uint8_t *data, uint16_t len) {
  if (ook_spi == NULL) {
    return E_OOK_NULLPTR_ERR;
  }

  ook_spi_tx_cplt_flag = false;
  HAL_StatusTypeDef ret = HAL_SPI_Transmit_DMA(ook_spi, data, len);
  while (!ook_spi_tx_cplt_flag) {
    goto_sleep();
  }

  if (ret == HAL_OK) {
    return E_OOK_SUCCESS;
  }

  return E_OOK_COM_ERR;
}

ook_err_t ook_spi_speed(enum phy_speed speed) {
  uint32_t prescaler = SPI_BAUDRATEPRESCALER_256;
  switch (speed) {
  case PHYSPEED_256:
    return E_OOK_CONFIG_ERR;
  case PHYSPEED_512:
    return E_OOK_CONFIG_ERR;
  case PHYSPEED_1024:
    prescaler = SPI_BAUDRATEPRESCALER_256;
    break;
  case PHYSPEED_2048:
    prescaler = SPI_BAUDRATEPRESCALER_128;
    break;
  case PHYSPEED_4096:
    prescaler = SPI_BAUDRATEPRESCALER_64;
    break;
  case PHYSPEED_8192:
    prescaler = SPI_BAUDRATEPRESCALER_32;
    break;
  case PHYSPEED_16384:
    prescaler = SPI_BAUDRATEPRESCALER_16;
    break;
  case PHYSPEED_32768:
    prescaler = SPI_BAUDRATEPRESCALER_8;
    break;
  }

  // Set Baudrate
  MODIFY_REG(ook_spi->Instance->CR1, SPI_BAUDRATEPRESCALER_256, prescaler);

  return E_OOK_SUCCESS;
}

ook_err_t ook_spi_enable(bool enable) {
  if (enable == true) {
    // Enable MOSI
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = OOK_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(OOK_SDA_GPIO_Port, &GPIO_InitStruct);
  }

  else if (enable == false) {
    // Disable MOSI and SCK
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DISABLE_IN_SW_Pin | OOK_SDA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(OOK_SDA_GPIO_Port, OOK_SDA_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DISABLE_IN_SW_GPIO_Port, DISABLE_IN_SW_Pin,
                      GPIO_PIN_RESET);
    HAL_GPIO_Init(OOK_SDA_GPIO_Port, &GPIO_InitStruct);
  }

  return E_OOK_SUCCESS;
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
  HAL_ResumeTick(); // resume tick in case IRQ was fired by a shutdown impulse
  if (hspi == wur_spi) {
    wur_spi_tx_cplt_flag = true;
  } else if (hspi == ook_spi) {
    ook_spi_tx_cplt_flag = true;
  }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi) {
  HAL_ResumeTick(); // resume tick in case IRQ was fired by a shutdown impulse
  if (hspi == wur_spi) {
    wur_spi_rx_cplt_flag = true;
  }
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin) {
  UNUSED(GPIO_Pin);
  HAL_ResumeTick(); // resume tick in case IRQ was fired by a shutdown impulse

  ULOG_INF("Got EXTI Callback - Resetting I2C such that shudown is possible");

  // Reset I2C bus in case it got stuck
  RCC->APBRSTR1 |= RCC_APBRSTR1_I2C3RST;
  RCC->APBRSTR1 &= ~RCC_APBRSTR1_I2C3RST;
}

// Logic Functions
void goto_shutdown(bool sys_irq) {

  ULOG_INF("Entering Shutdown");

#ifdef DEBUG
  HAL_Delay(1000);
#endif

  // Store all backup registers
  write_backup_reg();

  // FH101RF disable IRQs
  fh101rf_clear_irq(&fh101rf_dev);

  // Set antenna to FH101RF
  set_ant_control(FH101RF_DEV);

  //HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_A, SYS_SDN_Pin);

  HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_A, WUR_CS_Pin);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, WUR_MOSI_Pin);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, WUR_MISO_Pin);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, WUR_CLK_Pin);
  HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_A, WUR_RST_Pin);
  // HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, WUR_IRQ_Pin);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, OOK_SDA_Pin);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_A, DISABLE_IN_SW_Pin);

  if (sys_irq) {
    HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_B, SYS_IRQ_Pin);
  } else {
    HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_B, SYS_IRQ_Pin);
  }

  HAL_PWREx_EnablePullUpPullDownConfig();

  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_LOW);  // SYS_SDN on low
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN4_HIGH); // WUR_IRQ on high

  __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1); // Clear SYS_SDN wakeup flag
  __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF4); // Clear WUR_IRQ wakeup flag

#ifdef DEBUG
  HAL_DBGMCU_EnableDBGStandbyMode();
  HAL_PWR_EnterSTANDBYMode();
#else
  HAL_PWR_EnterSTANDBYMode();
#endif
}

// Function need to be really fast, else DMA might be finished before sleep is
// entered
void goto_sleep(void) {
  HAL_SuspendTick();
  HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
  HAL_ResumeTick();
}

enum wusrc check_wu_src(void) {
  if (__HAL_PWR_GET_FLAG(PWR_FLAG_WUF1) != RESET) { // If wake up from SYS_SDN
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF1);
    return WUSRC_SYS_SDN;
  } else if (__HAL_PWR_GET_FLAG(PWR_FLAG_WUF4) !=
             RESET) { // If wake up from WUR_IRQ
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF4);
    return WUSRC_WUR_IRQ;
  } else { // Else
    return WUSRC_RST;
  }
}

enum pinstate check_pinstate(void) {
  // Assume pin state is still the same after wake up, so distinguishing works!
  if (!HAL_GPIO_ReadPin(SYS_SDN_GPIO_Port, SYS_SDN_Pin)) {
    return PINSTATE_SYS_SDN;
  } else if (HAL_GPIO_ReadPin(WUR_IRQ_GPIO_Port, WUR_IRQ_Pin)) {
    return PINSTATE_WUR_IRQ;
  }
  return PINSTATE_NONE;
}

fh101rf_err_t handle_wur_irq(void) {
  // Reset last_irq_data
  memset((void *)&last_irq_data, 0, sizeof(struct wur_irq_data));

  // Read IRQ status from fh101rf
  uint8_t temp = 0;
  fh101rf_err_t err =
      fh101rf_read_reg(&fh101rf_dev, FH101RF_IRQ_STATUS_ADDRESS, &temp);
  if (err != E_FH101RF_SUCCESS) {
    return err;
  }

  // Unpack into struct
  struct fh101rf_irq_source temp_wur_irq = fh101rf_irq_source_unpack_be(&temp);
  last_irq_data.irq_source = temp;
  ULOG_INF("Source %d", temp);
  // Check if there is fifo data to readout
  if (temp_wur_irq.fifo_full || temp_wur_irq.id_match_and_fifo_full) {
    // Readout FIFO
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_0_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[0]);
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_1_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[1]);
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_2_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[2]);
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_3_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[3]);
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_4_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[4]);
    HAL_Delay(6); // somehow here a short delay of not less than 5ms is needed,
                  // as else FIFO has wrong data
    err |= fh101rf_read_reg(&fh101rf_dev, FH101RF_RX_FIFO_5_868_ADDRESS,
                            (uint8_t *)&last_irq_data.data[5]);
    if (err != E_FH101RF_SUCCESS) {
      return err;
    }

    // Clear counter
    err |= fh101rf_write_reg(&fh101rf_dev, FH101RF_FIFO_COUNT_868_ADDRESS, 0);
    if (err != E_FH101RF_SUCCESS) {
      ULOG_WRN("Failed to clear counter");
    }

    ULOG_DBG("Got FIFO-data from wake-up call");
    ULOG_DBG("%d, %d, %d, %d, %d, %d", last_irq_data.data[0],
             last_irq_data.data[1], last_irq_data.data[2],
             last_irq_data.data[3], last_irq_data.data[4],
             last_irq_data.data[5]);

    // If no error on readout, set data flag to true
    last_irq_data.got_data = 1;
  }

  return err;
}

// return sys_irq level which should be set
bool handle_wakeup(enum wusrc wakeup_source) {
  switch (wakeup_source) {
  case WUSRC_WUR_IRQ:
    ULOG_DBG("Got IRQ from WUR");
    if (handle_wur_irq() != E_FH101RF_SUCCESS) {
      ULOG_ERR("Error when reading out FH101RF IRQ Status");
      return false;
    }
    return true;
    break;
  case WUSRC_RST:
    ULOG_DBG("Got IRQ from RST. Doing nothing.");
    return false;
    break;
  case WUSRC_SYS_SDN:
    ULOG_DBG("Got IRQ from SDN");
    HAL_I2C_EnableListen_IT(&hi2c3);

    while (HAL_GPIO_ReadPin(SYS_SDN_GPIO_Port, SYS_SDN_Pin) == 0 ||
           lwrb_get_full(&i2c_read_ring_buffer) != 0) {
      // Wait for I2C to put something in buffer. In the end, expect SYS_SDN to
      // put module to shutdown again
      if (lwrb_get_full(&i2c_read_ring_buffer) == 0) {
        goto_sleep();
        continue;
      }

      // Readout data from ring buffer
      struct i2c_read_message msg = {0};
      size_t readed_bytes = lwrb_read(&i2c_read_ring_buffer, (void *)&msg,
                                      sizeof(struct i2c_read_message));
      if (readed_bytes != sizeof(struct i2c_read_message)) {
        ULOG_ERR("Amount of bytes inside the ring buffer is wrong: %d",
                 readed_bytes);
        continue;
      }

      // Handle them
      // setup WUR
      if (msg.reg == 0x01) {
        // Set receiver address
        fh101rf_dev.conf.id_lo.data = msg.data[1];
        fh101rf_dev.conf.id_hi.data = msg.data[0];

        // Set low data rate
        fh101rf_dev.conf.nfa868_slow.data =
            ook_speed_to_fh101rf_sample_rate((enum phy_speed)msg.data[2]);

        // Set fast data rate
        fh101rf_dev.conf.nfa868_fast.data =
            ook_speed_to_fh101rf_sample_rate((enum phy_speed)msg.data[3]);

        // Set IRQ source
        struct fh101rf_irq_source temp =
            fh101rf_irq_source_unpack_be(&msg.data[4]);
        memcpy(&fh101rf_dev.conf.irq_select.irq_select, &temp,
               sizeof(struct fh101rf_irq_select));

        // Set WakeMod IDM_CTRL
        fh101rf_dev.conf.idm_ctrl = fh101rf_idm_ctrl_unpack_be(&msg.data[5]);

        // Set WakeMod Branch
        uint8_t branch = msg.data[5] >> 2;
        fh101rf_dev.conf.band_branch_ctrl.active_branches = fh101rf_branches_unpack_be(&branch);
        // Setup WuR with this settings
        fh101rf_err_t err = fh101rf_init(&fh101rf_dev);
        if (err != E_FH101RF_SUCCESS) {
          ULOG_ERR("Error when configuring FH101RF: %d", err);
        } else {
          ULOG_INF("Setup WuR finished");
        }
      }
      // send WUC
      else if (msg.reg == 0x02) {
        // Set antenna control
        set_ant_control(OOK_DEV);

        // Allocate enough space, even if we do not need it - anyway it's
        // temporary
        uint8_t data[WUC_ADDR_LENGTH + WUC_DATA_LENGTH] = {
            0}; // 2 address and max 48bit data

        // Copy data
        data[0] = msg.data[0]; // id
        data[1] = msg.data[1]; // id
        // If additional data, copy
        if (msg.data[4] != 0) {
          memcpy(&data[2], &msg.data[5], 5);
        }

        enum phy_symbol fast_pattern[8 * (WUC_ADDR_LENGTH + WUC_DATA_LENGTH)] =
            {0};
        size_t fast_pattern_len = ook_bytes_to_physymbols(
            data,
            msg.data[4] != 0 ? WUC_ADDR_LENGTH + WUC_DATA_LENGTH
                             : WUC_ADDR_LENGTH,
            fast_pattern, sizeof(fast_pattern));

        struct wuc_pattern pattern = {.slow_pattern = BIT_ZERO,
                                      .slow_speed = (enum phy_speed)msg.data[2],
                                      .fast_pattern = fast_pattern,
                                      .fast_pattern_len = fast_pattern_len,
                                      .fast_speed =
                                          (enum phy_speed)msg.data[3]};
        // Send wake up pattern
        ook_send_wuc(&ook_dev, pattern);

        // Reset antenna control
        set_ant_control(FH101RF_DEV);
        ULOG_INF("Sending WuC finished");
      }
    }
    return false;
    break;
  }

  return false;
}

void read_backup_reg(void) {
  uint32_t bkup0 = READ_REG(TAMP->BKP0R);
  uint32_t bkup1 = READ_REG(TAMP->BKP1R);

  // Unpack bytes from bkup0 (assuming little-endian register storage)
  last_irq_data.irq_source = (uint8_t)(bkup0 & 0xFF);      // Bits 0-7
  last_irq_data.got_data = (uint8_t)((bkup0 >> 8) & 0xFF); // Bits 8-15
  last_irq_data.data[0] = (uint8_t)((bkup0 >> 16) & 0xFF); // Bits 16-23
  last_irq_data.data[1] = (uint8_t)((bkup0 >> 24) & 0xFF); // Bits 24-31

  // Unpack bytes from bkup1 (assuming little-endian register storage)
  last_irq_data.data[2] = (uint8_t)(bkup1 & 0xFF);         // Bits 0-7
  last_irq_data.data[3] = (uint8_t)((bkup1 >> 8) & 0xFF);  // Bits 8-15
  last_irq_data.data[4] = (uint8_t)((bkup1 >> 16) & 0xFF); // Bits 16-23
  last_irq_data.data[5] = (uint8_t)((bkup1 >> 24) & 0xFF); // Bits 24-31
}
void write_backup_reg(void) {
  // Pack bytes into uint32_t for bkup0 (assuming little-endian register
  // storage)
  uint32_t bkup0 = ((uint32_t)last_irq_data.data[1] << 24) |
                   ((uint32_t)last_irq_data.data[0] << 16) |
                   ((uint32_t)last_irq_data.got_data << 8) |
                   ((uint32_t)last_irq_data.irq_source);

  // Pack bytes into uint32_t for bkup1 (assuming little-endian register
  // storage)
  uint32_t bkup1 = ((uint32_t)last_irq_data.data[5] << 24) |
                   ((uint32_t)last_irq_data.data[4] << 16) |
                   ((uint32_t)last_irq_data.data[3] << 8) |
                   ((uint32_t)last_irq_data.data[2]);

  WRITE_REG(TAMP->BKP0R, bkup0);
  WRITE_REG(TAMP->BKP1R, bkup1);
}
