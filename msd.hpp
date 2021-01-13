//
// Multi-Stage Decimator for decimating a IQ stream from a RTL dongle.
//
// @author Johan Hedin
// @date   2021-01-13
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#ifndef MSD_HPP
#define MSD_HPP

#include <vector>
#include <iqsample.hpp>

// Multi-Stage Decimator
class MSD {
public:
    // Configuration for one stage. We need to know the decimation factor and
    // the coefficients that make up the FIR low pass filter
    struct Stage {
        unsigned           m;    // Decimation factor
        std::vector<float> coef; // Low pass filter FIR coefficients
    };

    // Construct a MSD. Argument is a list of stage configurations
    MSD(const std::vector<MSD::Stage> &stages) : m_(1), i_mean_lvl_(0.0f), q_mean_lvl_(0.0f), overload_(false), overload_count_(0) {
        auto iter = stages.begin();
        while (iter != stages.end()) {
            stages_.push_back(MSD::S(iter->m, iter->coef));
            m_ = m_ * iter->m;
            ++iter;
        }
    }

    // Get decimation factor for the MSD
    unsigned m(void) { return m_; }

    // I and Q mean level based on the last call to decimate
    float iMean(void) { return i_mean_lvl_; }
    float qMean(void) { return q_mean_lvl_; }
    bool overloaded(void) {
        auto tmp = overload_;

        if (overload_) overload_ = false;
        return tmp;
    }
    unsigned overloadCnt(void) {
        auto tmp = overload_count_;
        if (overload_count_ > 0) overload_count_ = 0;
        return tmp;
    }

    void decimate(const unsigned char *in, unsigned in_len, iqsample_t *out, unsigned *out_len) {
        unsigned sample_pos = 0;
        iqsample_t sample;
        float i, q;

        i_mean_lvl_ = 0.0f;
        q_mean_lvl_ = 0.0f;

        *out_len = 0;

        while (sample_pos < in_len) {
            // Convert two 8-bit in values to one iqsample in the range (-1.0 1.0)
            i = (float)in[sample_pos];
            q = (float)in[sample_pos+1];

#define CLIP_HI 255
#define CLIP_LO 0
            if (in[sample_pos] <= CLIP_LO || in[sample_pos] >= CLIP_HI  || in[sample_pos+1] <= CLIP_LO || in[sample_pos+1] >= CLIP_HI) {
                overload_ = true;
                overload_count_++;
            }

            i = i/127.5f - 1.0f;
            q = q/127.5f - 1.0f;

            i_mean_lvl_ += std::abs(i);
            q_mean_lvl_ += std::abs(q);

            sample = iqsample_t(i, q);

            auto stage_iter = stages_.begin();
            while (stage_iter != stages_.end()) {
                if (stage_iter->addSample(sample)) {
                    // The stage produced a out sample
                    sample = stage_iter->calculateOutput();
                } else {
                    // The stage need more samples
                    break;
                }
                ++stage_iter;
            }

            if (stage_iter == stages_.end()) {
                out[*out_len] = sample;
                *out_len += 1;
            }

            sample_pos += 2;
        }

        i_mean_lvl_ = i_mean_lvl_ / in_len;
        q_mean_lvl_ = q_mean_lvl_ / in_len;
    }

private:
    // Internal class representing one stage with its associated delay line
    // in the form of a ring buffer
    class S {
    public:
        S(unsigned m, const std::vector<float> c) : m_(m), c_(c),
            rb_(c.size(), iqsample_t(0.0f, 0.0f)), pos_(0), isn_(m) {}

        // Add one new sample to the delay line
        bool addSample(iqsample_t sample) {
            bool ret = false;

            // Add sample to the ring buffer at current position
            rb_[pos_] = sample;

            // Advance and if at the end, wrap around
            if (++pos_ == rb_.size()) pos_ = 0;

            // Decrease samples needed. If 0, we have enough new in samples
            // to calculate one output sample
            if (--isn_ == 0) {
                isn_ = m_;
                ret = true;
            }

            return ret;
        }

        // Calculat one output sample based on the in samples in the delay line
        // and the filter coefficients
        iqsample_t calculateOutput(void) {
            auto i = pos_;

            iqsample_t out_sample(0.0f, 0.0f);
            for (auto c = c_.begin(); c != c_.end(); ++c) {
                out_sample += *c * rb_[i++];
                if (i == rb_.size()) i = 0;
            }

            return out_sample;
        }

    private:
        unsigned                 m_;     // Decimation factor
        const std::vector<float> c_;     // FIR coefficients
        std::vector<iqsample_t>  rb_;    // Calculation ring buffer
        unsigned                 pos_;   // Current position in the ring buffer
        unsigned                 isn_;   // New in-samples needed before an output sample can be calculated
    };

    std::vector<MSD::S> stages_; // List of stages
    unsigned            m_;      // Total decimation factor
    float               i_mean_lvl_;
    float               q_mean_lvl_;
    bool                overload_;
    unsigned            overload_count_;
};

#endif // MSD_HPP
