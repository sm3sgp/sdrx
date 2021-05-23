
#include <cassert>
#include <chrono>
#include <iostream>

#include <stdio.h>

#include <rtl-sdr.h>

#include "rtl_dev.hpp"

#define MIN_FQ 45000000
#define MAX_FQ 1700000000

#define MIN_GAIN 0.0f
#define MAX_GAIN 50.0f

#define DEFAULT_FQ          100000000
#define DEFAULT_GAIN        30.0f


RtlDev::RtlDev(const std::string &serial, uint32_t fs, int xtal_corr)
: serial_(serial), fs_(fs), xtal_corr_(xtal_corr), fq_(DEFAULT_FQ),
  gain_(DEFAULT_GAIN), dev_(nullptr), run_(false), counter_(0)  {}


RtlDev::~RtlDev(void) {
    assert(run_ == false);
    assert(dev_ == nullptr);
}


int RtlDev::open(void) {
    assert(dev_ == nullptr);

    return open_();
}


int RtlDev::start() {
    assert(dev_ != nullptr);
    assert(run_ == false);
    run_ = true;
    worker_thread_ = std::thread(worker_, std::ref(*this));

    return RTLDEV_OK;
}


int RtlDev::stop() {
    assert(run_ == true);
    run_ = false;
    worker_thread_.join();

    return RTLDEV_OK;
}


int RtlDev::close(void) {
    assert(run_ == false);

    // Device might have bailed out in the worker thread so we only call
    // rtlsdr_close if device is still open
    if (dev_ != nullptr) {
        rtlsdr_close((rtlsdr_dev_t*)dev_);
        dev_ = nullptr;
    }

    return RTLDEV_OK;
}


int RtlDev::setFq(uint32_t fq) {
    if (fq < MIN_FQ || fq > MAX_FQ) return RTLDEV_INVALID_FQ;

    fq_ = fq;

    if (dev_) {
        rtlsdr_set_center_freq((rtlsdr_dev_t*)dev_, fq_);
    }

    return RTLDEV_OK;
}


int RtlDev::setTunerGain(float gain) {
    if (gain < MIN_GAIN || gain > MAX_GAIN) return RTLDEV_INVALID_GAIN;

    gain_ = gain;

    if (dev_) {
        rtlsdr_set_tuner_gain((rtlsdr_dev_t*)dev_, (int)(gain_ * 10.0f));
    }

    return RTLDEV_OK;
}


void RtlDev::worker_(RtlDev &self) {
#define RTL_IQ_BUF_SIZE    (512 * 75)
#define RTL_NUM_IQ_BUFFERS (4)
    int ret;

    while (self.run_) {
        rtlsdr_reset_buffer((rtlsdr_dev_t*)self.dev_);
        ret = rtlsdr_read_async((rtlsdr_dev_t*)self.dev_, RtlDev::data_cb_, &self, RTL_NUM_IQ_BUFFERS, RTL_IQ_BUF_SIZE);
        if (ret < 0 && self.run_ && self.serial_.length() != 0) {
            std::cerr << "Device " << self.serial_ << " disappeared. Trying to reopen...\n";
            rtlsdr_close((rtlsdr_dev_t*)self.dev_);
            self.dev_ = nullptr;

            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                ret = self.open_();
            } while (self.run_ && ret != RTLDEV_OK);

            if (self.run_ && ret == RTLDEV_OK) {
                std::cerr << "Device " << self.serial_ << " reopended successfully\n";
            }
        }
    }
}


int RtlDev::open_(void) {
    int      ret = 0;
    uint32_t id = 0;

    // Empty string mean device 0 (a.k.a the first one)
    if (serial_.length() != 0) {
        int tmp_id = rtlsdr_get_index_by_serial(serial_.c_str());
        if (tmp_id < 0) return RTLDEV_NO_DEVICE_FOUND;

        id = (uint32_t)tmp_id;
    }

    ret = rtlsdr_open((rtlsdr_dev_t**)&dev_, id);
    if (ret < 0) {
        dev_ = nullptr;
        return RTLDEV_UNABLE_TO_OPEN_DEVICE;
    }

    rtlsdr_set_center_freq((rtlsdr_dev_t*)dev_, fq_);
    rtlsdr_set_freq_correction((rtlsdr_dev_t*)dev_, xtal_corr_);
    rtlsdr_set_tuner_gain((rtlsdr_dev_t*)dev_, (int)(gain_ * 10.0f));
    rtlsdr_set_sample_rate((rtlsdr_dev_t*)dev_, fs_);

    return RTLDEV_OK;
}


void RtlDev::data_cb_(unsigned char *, uint32_t , void *ctx) {
    RtlDev &self = *reinterpret_cast<RtlDev*>(ctx);

    if (!self.run_) {
        rtlsdr_cancel_async((rtlsdr_dev_t*)self.dev_);
        return;
    }

    if (++self.counter_ == 60) {
        std::cout << "Device " << self.serial_ << ": data_cb" << std::endl;
        self.counter_ = 0;
    }
}


std::string RtlDev::errStr(int ret) {
    switch (ret) {
        case RTLDEV_OK:                    return "Ok";
        case RTLDEV_ERROR:                 return "Error";
        case RTLDEV_NO_DEVICE_FOUND:       return "No device found";
        case RTLDEV_UNABLE_TO_OPEN_DEVICE: return "Unable to open device";
        case RTLDEV_INVALID_FQ:            return "Invalid frequency";
        case RTLDEV_INVALID_GAIN:          return "Invalid gain";
        default:                           return "Unknown";
    }
}


std::vector<RtlDev::Info> RtlDev::list(void) {
#define RLT_STR_MAX_LEN 256
    int                 ret = 0;
    char                manufacturer[RLT_STR_MAX_LEN+1];
    char                product[RLT_STR_MAX_LEN+1];
    char                serial[RLT_STR_MAX_LEN+1];
    std::vector<Info>   devices;
    rtlsdr_dev_t       *rtl_device = nullptr;
    uint32_t            num_devices;

    num_devices = rtlsdr_get_device_count();
    for (uint32_t d = 0; d < num_devices; d++) {
        ret = rtlsdr_get_device_usb_strings(d, manufacturer, product, serial);
        if (ret < 0) {
            break;
        }

        manufacturer[RLT_STR_MAX_LEN] = 0;
        product[RLT_STR_MAX_LEN] = 0;
        serial[RLT_STR_MAX_LEN] = 0;

        Info info;
        info.serial = serial;
        info.index = (unsigned)d;
        info.available = false;
        info.supported = false;
        info.description = manufacturer;
        info.description += " ";
        info.description += product;

        // We need to open the device to check for xtal fq and tuner model
        ret = rtlsdr_open(&rtl_device, d);
        if (ret == 0) {
            info.available = true;

            // Verify 28.8MHz xtal and R820T(2) tuner
            uint32_t rtl2832_clk_fq = 0;
            uint32_t tuner_clk_fq = 0;
            enum rtlsdr_tuner tuner_type;

            rtlsdr_get_xtal_freq(rtl_device, &rtl2832_clk_fq, &tuner_clk_fq);
            tuner_type = rtlsdr_get_tuner_type(rtl_device);

            if (tuner_type == RTLSDR_TUNER_R820T && rtl2832_clk_fq == 28800000) {
                info.supported = true;
                info.sample_rates.push_back(1200000);
                info.sample_rates.push_back(2400000);
            }

            rtlsdr_close(rtl_device);
        }

        devices.push_back(info);
    }

    return devices;
}


bool RtlDev::present(const std::string &serial) {
#define RLT_STR_MAX_LEN 256
    uint32_t            num_devices;
    char                serial_str[RLT_STR_MAX_LEN+1];
    int                 ret = 0;

    num_devices = rtlsdr_get_device_count();
    for (uint32_t d = 0; d < num_devices; d++) {
        ret = rtlsdr_get_device_usb_strings(d, nullptr, nullptr, serial_str);
        if (ret < 0) {
            break;
        }

        serial_str[RLT_STR_MAX_LEN] = 0;

        if (serial == serial_str) return true;
    }

    return false;
}
