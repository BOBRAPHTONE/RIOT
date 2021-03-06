/*
 * Copyright (C) 2016 Fundación Inria Chile
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     drivers_sx127x
 * @{
 * @file
 * @brief       Netdev adaptation for the sx127x driver
 *
 * @author      Eugene P. <ep@unwds.com>
 * @author      José Ignacio Alamos <jose.alamos@inria.cl>
 * @}
 */

#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "net/netopt.h"
#include "net/netdev.h"
#include "sx127x_registers.h"
#include "sx127x_internal.h"
#include "sx127x_netdev.h"
#include "sx127x.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

/* Internal helper functions */
static uint8_t _get_tx_len(const struct iovec *vector, unsigned count);
static int _set_state(sx127x_t *dev, netopt_state_t state);
static int _get_state(sx127x_t *dev, void *val);

/* Netdev driver api functions */
static int _send(netdev_t *netdev, const struct iovec *vector, unsigned count);
static int _recv(netdev_t *netdev, void *buf, size_t len, void *info);
static int _init(netdev_t *netdev);
static void _isr(netdev_t *netdev);
static int _get(netdev_t *netdev, netopt_t opt, void *val, size_t max_len);
static int _set(netdev_t *netdev, netopt_t opt, void *val, size_t len);

const netdev_driver_t sx127x_driver = {
    .send = _send,
    .recv = _recv,
    .init = _init,
    .isr = _isr,
    .get = _get,
    .set = _set,
};

static int _send(netdev_t *netdev, const struct iovec *vector, unsigned count)
{
    sx127x_t *dev = (sx127x_t*) netdev;

    if (sx127x_get_state(dev) == SX127X_RF_TX_RUNNING) {
        DEBUG("[WARNING] Cannot send packet: radio alredy in transmitting "
              "state.\n");
        return -ENOTSUP;
    }

    uint8_t size;
    size = _get_tx_len(vector, count);
    switch (dev->settings.modem) {
        case SX127X_MODEM_FSK:
            /* todo */
            break;
        case SX127X_MODEM_LORA:
            /* Initializes the payload size */
            sx127x_set_payload_length(dev, size);

            /* Full buffer used for Tx */
            sx127x_reg_write(dev, SX127X_REG_LR_FIFOTXBASEADDR, 0x00);
            sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, 0x00);

            /* FIFO operations can not take place in Sleep mode
             * So wake up the chip */
            if (sx127x_get_op_mode(dev) == SX127X_RF_OPMODE_SLEEP) {
                sx127x_set_standby(dev);
                xtimer_usleep(SX127X_RADIO_WAKEUP_TIME); /* wait for chip wake up */
            }

            /* Write payload buffer */
            for (size_t i = 0;i < count ; i++) {
                sx127x_write_fifo(dev, vector[i].iov_base, vector[i].iov_len);
            }
            break;
        default:
            puts("sx127x_netdev, Unsupported modem");
            break;
    }

    /* Enable TXDONE interrupt */
    sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGSMASK,
                     SX127X_RF_LORA_IRQFLAGS_RXTIMEOUT |
                     SX127X_RF_LORA_IRQFLAGS_RXDONE |
                     SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR |
                     SX127X_RF_LORA_IRQFLAGS_VALIDHEADER |
                     /* SX127X_RF_LORA_IRQFLAGS_TXDONE | */
                     SX127X_RF_LORA_IRQFLAGS_CADDONE |
                     SX127X_RF_LORA_IRQFLAGS_FHSSCHANGEDCHANNEL |
                     SX127X_RF_LORA_IRQFLAGS_CADDETECTED);

    /* Set TXDONE interrupt to the DIO0 line */
    sx127x_reg_write(dev, SX127X_REG_DIOMAPPING1,
                     (sx127x_reg_read(dev, SX127X_REG_DIOMAPPING1) &
                      SX127X_RF_LORA_DIOMAPPING1_DIO0_MASK) |
                     SX127X_RF_LORA_DIOMAPPING1_DIO0_01);

    /* Start TX timeout timer */
    xtimer_set(&dev->_internal.tx_timeout_timer, dev->settings.lora.tx_timeout);

    /* Put chip into transfer mode */
    sx127x_set_state(dev, SX127X_RF_TX_RUNNING);
    sx127x_set_op_mode(dev, SX127X_RF_OPMODE_TRANSMITTER);

    return 0;
}

static int _recv(netdev_t *netdev, void *buf, size_t len, void *info)
{
    sx127x_t *dev = (sx127x_t*) netdev;
    volatile uint8_t irq_flags = 0;
    uint8_t size = 0;
    switch (dev->settings.modem) {
        case SX127X_MODEM_FSK:
            /* todo */
            break;
        case SX127X_MODEM_LORA:
            /* Clear IRQ */
            sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGS, SX127X_RF_LORA_IRQFLAGS_RXDONE);

            irq_flags = sx127x_reg_read(dev, SX127X_REG_LR_IRQFLAGS);
            if ( (irq_flags & SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR_MASK) ==
                 SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR) {
                /* Clear IRQ */
                sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGS,
                                 SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR);

                if (!(dev->settings.lora.flags & SX127X_RX_CONTINUOUS_FLAG)) {
                    sx127x_set_state(dev, SX127X_RF_IDLE);
                }

                xtimer_remove(&dev->_internal.rx_timeout_timer);
                netdev->event_callback(netdev, NETDEV_EVENT_CRC_ERROR);
                return -EBADMSG;
            }

            netdev_sx127x_lora_packet_info_t *packet_info = info;
            if (packet_info) {
                /* there is no LQI for LoRa */
                packet_info->lqi = 0;
                uint8_t snr_value = sx127x_reg_read(dev, SX127X_REG_LR_PKTSNRVALUE);
                if (snr_value & 0x80) { /* The SNR is negative */
                    /* Invert and divide by 4 */
                    packet_info->snr = -1 * ((~snr_value + 1) & 0xFF) >> 2;
                }
                else {
                    /* Divide by 4 */
                    packet_info->snr = (snr_value & 0xFF) >> 2;
                }

                int16_t rssi = sx127x_reg_read(dev, SX127X_REG_LR_PKTRSSIVALUE);

                if (packet_info->snr < 0) {
#if defined(MODULE_SX1272)
                    packet_info->rssi = SX127X_RSSI_OFFSET + rssi + (rssi >> 4) + packet_info->snr;
#else /* MODULE_SX1276 */
                    if (dev->settings.channel > SX127X_RF_MID_BAND_THRESH) {
                        packet_info->rssi = SX127X_RSSI_OFFSET_HF + rssi + (rssi >> 4) + packet_info->snr;
                    }
                    else {
                        packet_info->rssi = SX127X_RSSI_OFFSET_LF + rssi + (rssi >> 4) + packet_info->snr;
                    }
#endif
                }
                else {
#if defined(MODULE_SX1272)
                    packet_info->rssi = SX127X_RSSI_OFFSET + rssi + (rssi >> 4);
#else /* MODULE_SX1276 */
                    if (dev->settings.channel > SX127X_RF_MID_BAND_THRESH) {
                        packet_info->rssi = SX127X_RSSI_OFFSET_HF + rssi + (rssi >> 4);
                    }
                    else {
                        packet_info->rssi = SX127X_RSSI_OFFSET_LF + rssi + (rssi >> 4);
                    }
#endif
                }
                packet_info->time_on_air = sx127x_get_time_on_air(dev, len);
            }

            size = sx127x_reg_read(dev, SX127X_REG_LR_RXNBBYTES);
            if (buf == NULL) {
                return size;
            }

            if (size > len) {
                return -ENOBUFS;
            }

            if (!(dev->settings.lora.flags & SX127X_RX_CONTINUOUS_FLAG)) {
                sx127x_set_state(dev, SX127X_RF_IDLE);
            }

            xtimer_remove(&dev->_internal.rx_timeout_timer);

            /* Read the last packet from FIFO */
            uint8_t last_rx_addr = sx127x_reg_read(dev, SX127X_REG_LR_FIFORXCURRENTADDR);
            sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, last_rx_addr);
            sx127x_read_fifo(dev, (uint8_t*)buf, size);
            break;
        default:
            break;
    }

    return size;
}

static int _init(netdev_t *netdev)
{
    sx127x_t *sx127x = (sx127x_t*) netdev;

    sx127x->irq = 0;
    sx127x_radio_settings_t settings;
    settings.channel = SX127X_CHANNEL_DEFAULT;
    settings.modem = SX127X_MODEM_DEFAULT;
    settings.state = SX127X_RF_IDLE;

    sx127x->settings = settings;

    /* Launch initialization of driver and device */
    DEBUG("init_radio: initializing driver...\n");
    sx127x_init(sx127x);

    sx127x_init_radio_settings(sx127x);
    /* Put chip into sleep */
    sx127x_set_sleep(sx127x);

    DEBUG("init_radio: sx127x initialization done\n");

    return 0;
}

static void _isr(netdev_t *netdev)
{
    sx127x_t *dev = (sx127x_t *) netdev;

    uint8_t irq = dev->irq;
    dev->irq = 0;

    switch (irq) {
        case SX127X_IRQ_DIO0:
            sx127x_on_dio0(dev);
            break;

        case SX127X_IRQ_DIO1:
            sx127x_on_dio1(dev);
            break;

        case SX127X_IRQ_DIO2:
            sx127x_on_dio2(dev);
            break;

        case SX127X_IRQ_DIO3:
            sx127x_on_dio3(dev);
            break;

        default:
            break;
    }
}

static int _get(netdev_t *netdev, netopt_t opt, void *val, size_t max_len)
{
    sx127x_t *dev = (sx127x_t*) netdev;

    if (dev == NULL) {
        return -ENODEV;
    }

    switch(opt) {
        case NETOPT_STATE:
            assert(max_len >= sizeof(netopt_state_t));
            return _get_state(dev, val);

        case NETOPT_DEVICE_MODE:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = dev->settings.modem;
            return sizeof(uint8_t);

        case NETOPT_CHANNEL:
            assert(max_len >= sizeof(uint32_t));
            *((uint32_t*) val) = sx127x_get_channel(dev);
            return sizeof(uint32_t);

        case NETOPT_BANDWIDTH:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = sx127x_get_bandwidth(dev);
            return sizeof(uint8_t);

        case NETOPT_SPREADING_FACTOR:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = sx127x_get_spreading_factor(dev);
            return sizeof(uint8_t);

        case NETOPT_CODING_RATE:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = sx127x_get_coding_rate(dev);
            return sizeof(uint8_t);

        case NETOPT_MAX_PACKET_SIZE:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = sx127x_get_max_payload_len(dev);
            return sizeof(uint8_t);

        case NETOPT_INTEGRITY_CHECK:
            assert(max_len >= sizeof(netopt_enable_t));
            *((netopt_enable_t*) val) = sx127x_get_crc(dev) ? NETOPT_ENABLE : NETOPT_DISABLE;
            break;

        case NETOPT_CHANNEL_HOP:
            assert(max_len >= sizeof(netopt_enable_t));
            *((netopt_enable_t*) val) = (dev->settings.lora.flags & SX127X_CHANNEL_HOPPING_FLAG) ? NETOPT_ENABLE : NETOPT_DISABLE;
            break;

        case NETOPT_CHANNEL_HOP_PERIOD:
            assert(max_len >= sizeof(uint8_t));
            *((uint8_t*) val) = sx127x_get_hop_period(dev);
            return sizeof(uint8_t);

        case NETOPT_SINGLE_RECEIVE:
            assert(max_len >= sizeof(uint8_t));
            *((netopt_enable_t*) val) = sx127x_get_rx_single(dev) ? NETOPT_ENABLE : NETOPT_DISABLE;
            break;

        default:
            break;
    }

    return 0;
}

static int _set(netdev_t *netdev, netopt_t opt, void *val, size_t len)
{
    sx127x_t *dev = (sx127x_t*) netdev;
    int res = -ENOTSUP;

    if (dev == NULL) {
        return -ENODEV;
    }

    switch(opt) {
        case NETOPT_STATE:
            assert(len <= sizeof(netopt_state_t));
            return _set_state(dev, *((netopt_state_t*) val));

        case NETOPT_DEVICE_MODE:
            assert(len <= sizeof(uint8_t));
            sx127x_set_modem(dev, *((uint8_t*) val));
            return sizeof(netopt_enable_t);

        case NETOPT_CHANNEL:
            assert(len <= sizeof(uint32_t));
            sx127x_set_channel(dev, *((uint32_t*) val));
            return sizeof(uint32_t);

        case NETOPT_BANDWIDTH:
            assert(len <= sizeof(uint8_t));
            uint8_t bw = *((uint8_t *)val);
            if (bw < SX127X_BW_125_KHZ ||
                bw > SX127X_BW_500_KHZ) {
                res = -EINVAL;
                break;
            }
            sx127x_set_bandwidth(dev, bw);
            return sizeof(uint8_t);

        case NETOPT_SPREADING_FACTOR:
            assert(len <= sizeof(uint8_t));
            uint8_t sf = *((uint8_t *)val);
            if (sf < SX127X_SF6 ||
                sf > SX127X_SF12) {
                res = -EINVAL;
                break;
            }
            sx127x_set_spreading_factor(dev, sf);
            return sizeof(uint8_t);

        case NETOPT_CODING_RATE:
            assert(len <= sizeof(uint8_t));
            uint8_t cr = *((uint8_t *)val);
            if (cr < SX127X_CR_4_5 ||
                cr > SX127X_CR_4_8) {
                res = -EINVAL;
                break;
            }
            sx127x_set_coding_rate(dev, cr);
            return sizeof(uint8_t);

        case NETOPT_MAX_PACKET_SIZE:
            assert(len <= sizeof(uint8_t));
            sx127x_set_max_payload_len(dev, *((uint8_t*) val));
            return sizeof(uint8_t);

        case NETOPT_INTEGRITY_CHECK:
            assert(len <= sizeof(netopt_enable_t));
            sx127x_set_crc(dev, *((netopt_enable_t*) val) ? true : false);
            return sizeof(netopt_enable_t);

        case NETOPT_CHANNEL_HOP:
            assert(len <= sizeof(netopt_enable_t));
            sx127x_set_freq_hop(dev, *((netopt_enable_t*) val) ? true : false);
            return sizeof(netopt_enable_t);

        case NETOPT_CHANNEL_HOP_PERIOD:
            assert(len <= sizeof(uint8_t));
            sx127x_set_hop_period(dev, *((uint8_t*) val));
            return sizeof(uint8_t);

        case NETOPT_SINGLE_RECEIVE:
            assert(len <= sizeof(uint8_t));
            sx127x_set_rx_single(dev, *((netopt_enable_t*) val) ? true : false);
            return sizeof(netopt_enable_t);

        case NETOPT_RX_TIMEOUT:
            assert(len <= sizeof(uint32_t));
            sx127x_set_rx_timeout(dev, *((uint32_t*) val));
            return sizeof(uint32_t);

        case NETOPT_TX_TIMEOUT:
            assert(len <= sizeof(uint32_t));
            sx127x_set_tx_timeout(dev, *((uint32_t*) val));
            return sizeof(uint32_t);

        case NETOPT_TX_POWER:
            assert(len <= sizeof(uint8_t));
            sx127x_set_tx_power(dev, *((uint8_t*) val));
            return sizeof(uint16_t);

        case NETOPT_FIXED_HEADER:
            assert(len <= sizeof(netopt_enable_t));
            sx127x_set_fixed_header_len_mode(dev, *((netopt_enable_t*) val) ? true : false);
            return sizeof(netopt_enable_t);

        case NETOPT_PREAMBLE_LENGTH:
            assert(len <= sizeof(uint16_t));
            sx127x_set_preamble_length(dev, *((uint16_t*) val));
            return sizeof(uint16_t);

        case NETOPT_IQ_INVERT:
            assert(len <= sizeof(netopt_enable_t));
            sx127x_set_iq_invert(dev, *((netopt_enable_t*) val) ? true : false);
            return sizeof(bool);

        default:
            break;
    }

    return res;
}

static uint8_t _get_tx_len(const struct iovec *vector, unsigned count)
{
    uint8_t len = 0;

    for (int i=0 ; i < count ; i++) {
        len += vector[i].iov_len;
    }

    return len;
}

static int _set_state(sx127x_t *dev, netopt_state_t state)
{
    switch (state) {
        case NETOPT_STATE_SLEEP:
            sx127x_set_sleep(dev);
            break;

        case NETOPT_STATE_STANDBY:
            sx127x_set_standby(dev);
            break;

        case NETOPT_STATE_IDLE:
            dev->settings.window_timeout = 0;
            /* set permanent listening */
            sx127x_set_rx(dev);
            break;

        case NETOPT_STATE_RX:
            sx127x_set_rx(dev);
            break;

        case NETOPT_STATE_TX:
            sx127x_set_tx(dev);
            break;

        case NETOPT_STATE_RESET:
            sx127x_reset(dev);
            break;

        default:
            return -ENOTSUP;
    }
    return sizeof(netopt_state_t);
}

static int _get_state(sx127x_t *dev, void *val)
{
    uint8_t op_mode;
    op_mode = sx127x_get_op_mode(dev);
    netopt_state_t state;
    switch(op_mode) {
        case SX127X_RF_OPMODE_SLEEP:
            state = NETOPT_STATE_SLEEP;
            break;

        case SX127X_RF_OPMODE_STANDBY:
            state = NETOPT_STATE_STANDBY;
            break;

        case SX127X_RF_OPMODE_TRANSMITTER:
            state = NETOPT_STATE_TX;
            break;

        case SX127X_RF_OPMODE_RECEIVER:
        case SX127X_RF_LORA_OPMODE_RECEIVER_SINGLE:
            state = NETOPT_STATE_IDLE;
            break;

        default:
            break;
    }
    memcpy(val, &state, sizeof(netopt_state_t));
    return sizeof(netopt_state_t);
}
