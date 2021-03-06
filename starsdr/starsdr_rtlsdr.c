// based on SDRIO - Copyright Scott Cutler
// This source file is licensed under the GNU Lesser General Public License (LGPL)

/*
# (c) 2016 Outernet Inc
# This file is part of StarSDR.

# StarSDR is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesseer General Public License as 
# published by the Free Software Foundation, either version 3 of 
# the License, or (at your option) any later version.

# StarSDR is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.

# You should have received a copy of the GNU Lesser General Public 
# License along with StarSDR.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "starsdr_ext.h"

#define rtlsdr_STATIC
#include "rtl-sdr.h"

#include "pthread.h"

typedef struct starsdr_device_t
{
    starsdr_uint32 device_index;
    rtlsdr_dev_t *rtl_device;

    enum rtlsdr_tuner tuner;

    starsdr_int32 num_gains;
    starsdr_int32 *gains;

    starsdr_rx_async_callback callback;
    void *callback_context;
    pthread_t tid;

    starsdr_int16 *samples;
    starsdr_int32 num_samples;

    starsdr_uint64 min_freq;
    starsdr_uint64 max_freq;
} starsdr_device;

// must be multiple of 512!
#define DEFAULT_USB_BULK_BUFFER_SIZE ((128 * 512))
static int usb_bulk_buffer_size = DEFAULT_USB_BULK_BUFFER_SIZE;

STARSDREXPORT starsdr_int32 starsdr_init()
{
    return 1;
}

STARSDREXPORT starsdr_int32 starsdr_get_num_devices()
{
    return rtlsdr_get_device_count();
}

STARSDREXPORT starsdr_device * starsdr_open_device(starsdr_uint32 device_index)
{
    starsdr_device *dev = (starsdr_device *)malloc(sizeof(starsdr_device));

    if (dev)
    {
        memset(dev, 0, sizeof(starsdr_device));
        dev->device_index = device_index;

        rtlsdr_open(&dev->rtl_device, dev->device_index);

        if (dev->rtl_device)
        {
            dev->tuner = rtlsdr_get_tuner_type(dev->rtl_device);

            dev->num_gains = (starsdr_int32)rtlsdr_get_tuner_gains(dev->rtl_device, 0);
            if (dev->num_gains > 0)
            {
                dev->gains = (starsdr_int32 *)malloc(dev->num_gains * sizeof(starsdr_int32));
                rtlsdr_get_tuner_gains(dev->rtl_device, (int *)dev->gains);
            }
            else
            {
                free(dev);
                return 0;
            }

            starsdr_set_rx_samplerate(dev, 2048000);

            switch (rtlsdr_get_tuner_type(dev->rtl_device))
            {
            case RTLSDR_TUNER_E4000:  dev->min_freq = 52000000;  dev->max_freq = 2200000000u; break;
            case RTLSDR_TUNER_FC0012: dev->min_freq = 22000000;  dev->max_freq = 948600000; break;
            case RTLSDR_TUNER_FC0013: dev->min_freq = 22000000;  dev->max_freq = 1100000000; break;
            case RTLSDR_TUNER_FC2580: dev->min_freq = 146000000; dev->max_freq = 924000000; break;
            case RTLSDR_TUNER_R820T:  dev->min_freq = 24000000;  dev->max_freq = 1766000000; break;
            case RTLSDR_TUNER_R828D:  dev->min_freq = 24000000;  dev->max_freq = 1766000000; break;
            default: dev->min_freq = 0; dev->max_freq = 0; break;
            }
        }
        else
        {
            free(dev);
            return 0;
        }
    }

    return dev;
}

STARSDREXPORT starsdr_int32 starsdr_close_device(starsdr_device *dev)
{
    if (dev && dev->rtl_device)
    {
        int r = rtlsdr_close(dev->rtl_device);
        dev->rtl_device = 0;
        return r;
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT const char * starsdr_get_device_string(starsdr_uint32 device_index)
{
    return rtlsdr_get_device_name(device_index);
}

STARSDREXPORT starsdr_int32 starsdr_set_rx_samplerate(starsdr_device *dev, starsdr_uint64 sample_rate)
{
    if (dev)
    {
        return rtlsdr_set_sample_rate(dev->rtl_device, (uint32_t)sample_rate);
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int32 starsdr_set_rx_frequency(starsdr_device *dev, starsdr_uint64 frequency)
{
    if (dev)
    {
        return rtlsdr_set_center_freq(dev->rtl_device, (uint32_t)frequency);
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int32 starsdr_set_tx_samplerate(starsdr_device *dev, starsdr_uint64 sample_rate)
{
    return 0;
}

STARSDREXPORT starsdr_int32 starsdr_set_tx_frequency(starsdr_device *dev, starsdr_uint64 frequency)
{
    return 0;
}

void rtlsdr_read_async_cb(unsigned char *buf, uint32_t len, void *ctx)
{
    starsdr_device *dev = (starsdr_device *)ctx;

    if (dev->callback)
    {
        starsdr_int32 num_samples = len / (sizeof(starsdr_uint8)*2) ; // 2 bytes per sample

        if (dev->num_samples != num_samples)
        {
            dev->num_samples = num_samples;

            if (dev->samples)
            {
                free(dev->samples);
            }

            dev->samples = (starsdr_int16 *)malloc(dev->num_samples * sizeof(starsdr_int16) * 2);
        }

        if (dev->samples)
        {
            starsdr_uint32 i;
            starsdr_int16 *samples;

            for (i=0, samples=dev->samples; i< 2 * dev->num_samples; i++)
            {
		*(samples++) = (starsdr_int16) *(buf++) - 127;
            }

            dev->callback(dev->callback_context, dev->samples, dev->num_samples);
        }
    }
}

STARSDREXPORT void * start_rx_routine(void *ctx)
{
    starsdr_device *dev = (starsdr_device *)ctx;

    if (dev)
    {
        rtlsdr_reset_buffer(dev->rtl_device);
        rtlsdr_read_async(dev->rtl_device, rtlsdr_read_async_cb, (void *)dev, 0, usb_bulk_buffer_size);
        pthread_exit(0);
    }

    return 0;
}

STARSDREXPORT starsdr_int32 starsdr_start_rx(starsdr_device *dev, starsdr_rx_async_callback callback, void *context, int usb_buffer_num_samples)
{
    // if buffer size not a multiple of 256
    if (usb_buffer_num_samples % 256)
        return 0;

    // if passed-in size is 0, use DEFAULT size
    if (usb_buffer_num_samples > 0)
        usb_bulk_buffer_size = usb_buffer_num_samples * 2; // 1 complex sample = 2x uint8 values
    else
        usb_bulk_buffer_size = DEFAULT_USB_BULK_BUFFER_SIZE;

    if (dev)
    {
        dev->callback = callback;
        dev->callback_context = context;
        return pthread_create(&dev->tid, 0, start_rx_routine, (void *)dev) == 0;
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int32 starsdr_stop_rx(starsdr_device *dev)
{
    if (dev)
    {
        rtlsdr_cancel_async(dev->rtl_device);
        pthread_join(dev->tid, 0);
        return 1;
    }
    else
    {
        return 0;
    }

}

STARSDREXPORT starsdr_int32 starsdr_start_tx(starsdr_device *dev, starsdr_tx_async_callback callback, void *context)
{
    return 0;
}

STARSDREXPORT starsdr_int32 starsdr_stop_tx(starsdr_device *dev)
{
    return 0;
}

STARSDREXPORT starsdr_int64 starsdr_get_rx_frequency(starsdr_device *dev)
{
    if (dev)
    {
        return rtlsdr_get_center_freq(dev->rtl_device);
    }
    else
    {
        return 0;
    }
}

static const starsdr_uint32 sample_rates[] = {1024000, 1800000, 1920000, 2048000, 2400000, 2600000, 2800000, 3000000, 3200000};

STARSDREXPORT starsdr_int32 starsdr_get_num_samplerates(starsdr_device *dev)
{
    return sizeof(sample_rates) / sizeof(sample_rates[0]);
}

STARSDREXPORT void starsdr_get_samplerates(starsdr_device *dev, starsdr_uint32 *sample_rates_out)
{
    memcpy(sample_rates_out, sample_rates, sizeof(sample_rates));
}

STARSDREXPORT starsdr_int64 starsdr_get_rx_samplerate(starsdr_device *dev)
{
    if (dev)
    {
        return rtlsdr_get_sample_rate(dev->rtl_device);
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int64 starsdr_get_tx_frequency(starsdr_device *dev)
{
    return 0;
}

STARSDREXPORT starsdr_int64 starsdr_get_tx_samplerate(starsdr_device *dev)
{
    return 0;
}

STARSDREXPORT starsdr_int32 starsdr_set_rx_gain_mode(starsdr_device *dev, starsdr_gain_mode gain_mode)
{
    if (dev)
    {
        switch (gain_mode)
        {
        case starsdr_gain_mode_agc:
            rtlsdr_set_agc_mode(dev->rtl_device, 0);
            rtlsdr_set_tuner_gain_mode(dev->rtl_device, 0);
            break;
        case starsdr_gain_mode_manual:
            rtlsdr_set_agc_mode(dev->rtl_device, 0);
            rtlsdr_set_tuner_gain_mode(dev->rtl_device, 1);
            break;
        default:
            return 0;
        }
    }
    return 1;
}

STARSDREXPORT starsdr_int32 starsdr_get_rx_gain_range(starsdr_device *dev, starsdr_float32 *min, starsdr_float32 *max)
{
    if (dev && dev->gains)
    {
        if (min)
        {
            *min = 0.1f * (float)dev->gains[0];
        }

        if (max)
        {
            *max = 0.1f * (float)dev->gains[dev->num_gains-1];
        }

        return 1;
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int32 starsdr_set_rx_gain(starsdr_device *dev, starsdr_float32 gain)
{
    if (dev)
    {
        starsdr_int32 i;
        for (i=dev->num_gains-1; i>=0; i--)
        {
            if ((starsdr_int32)(gain * 10.0f) >= dev->gains[i])
            {
                rtlsdr_set_tuner_gain(dev->rtl_device, dev->gains[i]);
                return 1;
            }
        }

        if (dev->num_gains)
        {
            rtlsdr_set_tuner_gain(dev->rtl_device, dev->gains[0]);
            return 1;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return 0;
    }
}

STARSDREXPORT starsdr_int32 starsdr_get_tx_gain_range(starsdr_device *dev, starsdr_float32 *min, starsdr_float32 *max)
{
    return 0;
}

STARSDREXPORT starsdr_int32 starsdr_set_tx_gain(starsdr_device *dev, starsdr_float32 gain)
{
    return 0;
}

STARSDREXPORT void starsdr_get_tuning_range(starsdr_device *dev, starsdr_uint64 *min, starsdr_uint64 *max)
{
    if (dev)
    {
        if (min) *min = dev->min_freq;
        if (max) *max = dev->max_freq;
    }
}

STARSDREXPORT starsdr_int32 starsdr_get_caps(starsdr_device *dev, starsdr_caps caps)
{
    switch (caps)
    {
    case starsdr_caps_rx:  return  1;
    case starsdr_caps_tx:  return  0;
    case starsdr_caps_agc: return  1;
    default:             return -1;
    }
}


STARSDREXPORT starsdr_float32 starsdr_get_tuner_gain(starsdr_device *dev)
{
    return ((starsdr_float32) rtlsdr_get_tuner_gain(dev->rtl_device))/ 10.0f;
}


STARSDREXPORT starsdr_int32 starsdr_get_tuner_gains(starsdr_device *dev, starsdr_float32 *gains)
{
    starsdr_int32 i;
    int num_gains;
    int *igains = (int *) gains;
    num_gains = rtlsdr_get_tuner_gains(dev->rtl_device, igains);

    if (gains) 
    {
        for (i=0; i<num_gains; i++)
        {
            gains[i] = ((starsdr_float32)(igains[i]))/10.0;
        }
    }   
    return (starsdr_int32) num_gains;
}

STARSDREXPORT starsdr_int32 starsdr_get_sample_bitsize(starsdr_device *dev)
{
    return (starsdr_int32) 8;
}

