/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <miopen/rnn.hpp>
#include <miopen/rnn_util.hpp>

#include <miopen/activ.hpp>
#include <miopen/env.hpp>
#include <miopen/gemm_v2.hpp>
#include <miopen/logger.hpp>

#include <vector>
#include <numeric>
#include <algorithm>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_RNNFWD_exp)

namespace miopen {

void RNNDescriptor::RNNForwardTraining_MS(Handle& handle,
                                          std::vector<int>& seq_array,
                                          const TensorDescriptor& xDesc,
                                          ConstData_t x,
                                          const TensorDescriptor& hxDesc,
                                          ConstData_t hx,
                                          ConstData_t cx,
                                          const TensorDescriptor& wDesc,
                                          ConstData_t w,
                                          const TensorDescriptor& yDesc,
                                          Data_t y,
                                          Data_t hy,
                                          Data_t cy,
                                          Data_t reserveSpace,
                                          size_t reserveSpaceSize) const
{
#if MIOPEN_USE_GEMM && MIOPEN_BACKEND_HIP
    std::vector<int> in_n;
    int in_vec  = xDesc.GetLengths()[1]; // input vector size
    int out_vec = yDesc.GetLengths()[1]; // output vector size

    int seq_len   = seq_array.size();
    int max_batch = seq_array[0];
    int hidden_size;

    std::tie(std::ignore, max_batch, hidden_size) = miopen::tien<3>(hxDesc.GetLengths());

    auto extra_stream_cnt = 2;
    handle.ReserveExtraStreamsInPool(extra_stream_cnt);

    auto root_stream_id = 0;
    std::vector<hipStream_t> stream_pull;
    for(int i = 0; i <= extra_stream_cnt; i++)
    {
        handle.SetStreamFromPool(i);
        stream_pull.push_back(handle.GetStream());
    }

    handle.SetStreamFromPool(root_stream_id);

    int total_batch_size = 0;
    std::vector<int> bacc_per_time(seq_len + 1);

    for(int i = 0; i < seq_len; i++)
    {
        bacc_per_time[i] = total_batch_size;
        total_batch_size += seq_array[i];
        in_n.push_back(seq_array[i]);
    }
    bacc_per_time[seq_len] = total_batch_size;

    const struct
    {
        int batch;
    } InBuff_strides{in_vec};

    auto get_HxBuff_offset = [&](int layer_id) {
        return layer_id * (static_cast<size_t>(hidden_size) * max_batch);
    };

    int gates_cnt       = 4;
    int save_points_cnt = 6;

    struct WeightsBufferHelper
    {
    private:
        auto hidden_xinput_size(int hidden_sz, int bidirect_mode) const
        {
            if(bidirect_mode == 0)
                return hidden_sz;
            MIOPEN_THROW("execution failure: bidirect is not supported by this solver");
        }

        auto matrix_lin_layer_size(int input_vector_sz, int hidden_vec_sz, int gates) const
        {
            return (input_vector_sz + hidden_vec_sz) * hidden_vec_sz * gates;
        }
        size_t bias_start_offset(int input_vector_sz,
                                 int hidden_vec_sz,
                                 int layers_cnt,
                                 int gates,
                                 int bidirect_mode) const
        {
            if(bidirect_mode == 0)
                return matrix_lin_layer_size(input_vector_sz, hidden_vec_sz, gates) +
                       static_cast<size_t>(hidden_vec_sz + hidden_xinput_size(hidden_vec_sz, 0)) *
                           hidden_vec_sz * static_cast<size_t>(layers_cnt - 1) * gates;

            MIOPEN_THROW("execution failure: bidirect is not supported by this solver");
        }

    public:
        WeightsBufferHelper(
            int input_vector_sz, int hidden_vec_sz, int layers_cnt, int bias_mode, int gates)
            : in_vec(input_vector_sz),
              h_vec(hidden_vec_sz),
              x_in_vec(hidden_xinput_size(hidden_vec_sz, 0)),
              layers(layers_cnt),
              gates_cnt(gates),
              bias_cnt(bias_mode),
              matrix_normal_start_off(matrix_lin_layer_size(input_vector_sz, hidden_vec_sz, gates)),
              bias_start_off(
                  bias_start_offset(input_vector_sz, hidden_vec_sz, layers_cnt, gates, 0))
        {
        }

        const int in_vec, h_vec;
        const int x_in_vec; // for bidirect TODO

        const int layers;
        const int gates_cnt;
        const int
            bias_cnt; // 0 - no bisa; 1 - one bias; 2 - separate bias for x_vec and for hidden_vec
    private:
        const size_t matrix_normal_start_off;
        const size_t bias_start_off;

    public:
        auto get_matrix_x_size(int layer_id) const
        {
            return (layer_id > 0 ? x_in_vec : in_vec) * h_vec;
        }
        auto get_matrix_h_size() const { return h_vec * h_vec; }
        auto get_matrix_layer_size(int layer_id) const
        {
            return get_matrix_x_size(layer_id) * gates_cnt + get_matrix_h_size() * gates_cnt;
        }

        size_t get_matrix_x_off(int layer_id) const
        {
            if(layer_id > 0)
                return matrix_normal_start_off +
                       static_cast<size_t>(layer_id - 1) * get_matrix_layer_size(layer_id);
            else
                return 0;
        };

        size_t get_matrix_h_off(int layer_id) const
        {
            if(layer_id > 0)
                return get_matrix_x_off(layer_id) +
                       static_cast<size_t>(h_vec * x_in_vec * gates_cnt);
            else
                return get_matrix_x_off(layer_id) + static_cast<size_t>(h_vec * in_vec) * gates_cnt;
        };

        int bias_vector_size() const { return h_vec; }
        int bias_vector_mul_gate() const { return bias_vector_size() * gates_cnt; }
        int bias_stride() const { return bias_vector_mul_gate(); }

        size_t bias_relative_off(int layer_id, int bias_id) const
        {
            return static_cast<size_t>(layer_id * bias_cnt + bias_id) * gates_cnt * h_vec;
        }

        size_t get_bias_off(int layer_id, int bias_id) const
        {
            return bias_start_off + bias_relative_off(layer_id, bias_id);
        }

    } WeiBuf(in_vec, hidden_size, nLayers, biasMode * 2, gates_cnt);

    struct ReserveBufferHelper
    {
        struct RBuffHelper
        {
            int element, save_point, batch;
            size_t layer;
        };

    private:
        auto Reserve_Buffer_strides(int save_point_sz,
                                    int batches_per_layer,
                                    int save_points,
                                    int bidirect_mode = 0) const
        {
            const auto element_st    = 1;
            const auto save_point_st = element_st * save_point_sz;
            const auto batch_st      = save_point_st * save_points;
            const auto layer_st      = static_cast<size_t>(batch_st) * batches_per_layer;
            if(bidirect_mode == 0)
                return RBuffHelper{element_st, save_point_st, batch_st, layer_st};
            MIOPEN_THROW("execution failure: bidirect is not supported by this solver");
        }

    public:
        enum save_point
        {
            F  = 1,
            I  = 0,
            G  = 2,
            O  = 3,
            St = 4,
            Ht = 5
        };

        ReserveBufferHelper(int hidden_vec_sz,
                            int save_point_sz,
                            int layers_cnt,
                            int batches_per_layer,
                            int save_points,
                            int gates_cnt)
            : h_vec(hidden_vec_sz),
              save_point_size(save_point_sz),
              layers(layers_cnt),
              batches(batches_per_layer),
              save_points_cnt(save_points),
              gates(gates_cnt),
              strides(Reserve_Buffer_strides(save_point_sz, batches, save_points, 0))
        {
        }

        const int h_vec;
        const int save_point_size; // for bidirect TODO

        const int layers;
        const int batches;
        const int save_points_cnt;
        const int gates;
        const RBuffHelper strides;

        size_t layer_offset(int layer) const { return static_cast<size_t>(layer) * strides.layer; }
        auto layer_stride() const { return strides.layer; }

        auto gemm_write_size() const { return h_vec * gates; }
        auto gemm_write_stride() const
        {
            return strides.batch;
        } // save_point_size * save_points_cnt

        size_t gemm_write_relative_offset(int batch_id) const
        {
            return static_cast<size_t>(gemm_write_stride()) * batch_id;
        }

        size_t gemm_write_offset(int layer, int batch_id) const
        {
            return layer_offset(layer) + static_cast<size_t>(gemm_write_stride()) * batch_id;
        }

        auto ht_relative_offset() const { return save_point::Ht * save_point_size; }

        auto ct_relative_offset() const { return save_point::St * save_point_size; }

        auto get_gate_relative_offset(int gate_id) const { return gate_id * save_point_size; }

        size_t ht_offset(int layer_id, int batch_id) const
        {
            return layer_offset(layer_id) + gemm_write_relative_offset(batch_id) +
                   ht_relative_offset();
        }

        size_t extra_save_point_offset(int layer_id, int batch_id) const
        {
            return (static_cast<size_t>(batches) * layers * gemm_write_stride()) // all data offset
                   + (static_cast<size_t>(batches) * layer_id) * h_vec +
                   static_cast<size_t>(batch_id * h_vec);
        }

    } RBuff(hidden_size, hidden_size, nLayers, total_batch_size, save_points_cnt, gates_cnt);

    auto call_x_gemm = [&RBuff,
                        &WeiBuf,
                        &InBuff_strides,
                        &bacc_per_time,
                        &handle,
                        &xDesc,
                        reserveSpace,
                        x,
                        w,
                        hidden_size,
                        in_vec](int layer, int start_time, int time_cnt, float beta_t = 1) {
        const auto start_b  = bacc_per_time[start_time];
        const auto batch_sz = bacc_per_time[start_time + time_cnt] - start_b;

        const int m = batch_sz, n = RBuff.gemm_write_size(), k = layer > 0 ? hidden_size : in_vec;
        const int lda = layer > 0 ? RBuff.gemm_write_stride() : InBuff_strides.batch, ldb = k,
                  ldc = RBuff.gemm_write_stride();

        const miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                false,
                                                                true,
                                                                m,
                                                                n,
                                                                k,
                                                                lda,
                                                                ldb,
                                                                ldc,
                                                                1,      // batch count
                                                                0,      // Stride A
                                                                0,      // Stride B
                                                                0,      // Stride C
                                                                1,      // alpha
                                                                beta_t, // beta
                                                                xDesc.GetType(),
                                                                false};

        const auto wx_off     = WeiBuf.get_matrix_x_off(layer);
        const auto out_offset = RBuff.gemm_write_offset(layer, start_b);

        const auto x_in_offset = layer > 0 ? RBuff.ht_offset(layer - 1, start_b)
                                           : static_cast<size_t>(start_b * InBuff_strides.batch);
        const auto in_ptr      = layer > 0 ? reserveSpace : x;

        const miopenStatus_t gemm_status = CallGemm(handle,
                                                    gemm_desc,
                                                    in_ptr,
                                                    x_in_offset,
                                                    w,
                                                    wx_off,
                                                    reserveSpace,
                                                    out_offset,
                                                    GemmBackend_t::miopengemm);
        if(gemm_status != miopenStatusSuccess)
            MIOPEN_THROW("GEMM execution failure");
    };

    auto call_bias_add = [&RBuff, &WeiBuf, &handle, &wDesc, reserveSpace, w](int layer,
                                                                             float beta_t = 0) {
        float alpha0           = 1;
        float alpha1           = 1;
        const auto bias_stride = WeiBuf.bias_stride();

        const auto bias_desc =
            miopen::TensorDescriptor(wDesc.GetType(),
                                     std::vector<int>{1, 1, WeiBuf.bias_vector_mul_gate()},
                                     std::vector<int>{bias_stride, bias_stride, 1});

        const auto hidden_interim_desc = miopen::TensorDescriptor(
            wDesc.GetType(),
            std::vector<int>{1, RBuff.batches, WeiBuf.bias_vector_mul_gate()},
            std::vector<int>{
                RBuff.batches * RBuff.gemm_write_stride(), RBuff.gemm_write_stride(), 1});

        const auto RB_layer_out_off       = RBuff.layer_offset(layer);
        const auto w_bias_layer_start_off = WeiBuf.get_bias_off(layer, 0);

        OpTensor(handle,
                 miopenTensorOpAdd,
                 &alpha0,
                 hidden_interim_desc,
                 reserveSpace, // A
                 &alpha1,
                 bias_desc,
                 w, // B
                 &beta_t,
                 hidden_interim_desc,
                 reserveSpace,           // C
                 RB_layer_out_off,       // A offset
                 w_bias_layer_start_off, // B offset
                 RB_layer_out_off);      // C offset

        OpTensor(handle,
                 miopenTensorOpAdd,
                 &alpha0,
                 hidden_interim_desc,
                 reserveSpace,
                 &alpha1,
                 bias_desc,
                 w,
                 &beta_t,
                 hidden_interim_desc,
                 reserveSpace,
                 RB_layer_out_off,
                 w_bias_layer_start_off + bias_stride,
                 RB_layer_out_off);
    };

    auto call_hx_gemm = [&RBuff,
                         &WeiBuf,
                         &get_HxBuff_offset,
                         &bacc_per_time,
                         &in_n,
                         &handle,
                         &xDesc,
                         reserveSpace,
                         hx,
                         w,
                         hidden_size](int layer, int cur_time) {
        const int m = in_n.at(cur_time), n = RBuff.gemm_write_size(), k = hidden_size;

        const int lda = (cur_time != 0) ? RBuff.gemm_write_stride() : hidden_size,
                  ldb = hidden_size, ldc = RBuff.gemm_write_stride();

        const auto hx_ptr_offset = (cur_time == 0)
                                       ? get_HxBuff_offset(layer)
                                       : RBuff.ht_offset(layer, bacc_per_time[cur_time - 1]);

        if(cur_time == 0)
            if(hx == nullptr)
                return;

        const miopen::GemmDescriptor gemm_desc_hx = GemmDescriptor{false,
                                                                   false,
                                                                   true,
                                                                   m,
                                                                   n,
                                                                   k,
                                                                   lda,
                                                                   ldb,
                                                                   ldc,
                                                                   1, // batch count
                                                                   0, // Stride A
                                                                   0, // Stride B
                                                                   0, // Stride C
                                                                   1, // alpha
                                                                   1, // beta
                                                                   xDesc.GetType(),
                                                                   false};

        const auto RB_layer_save_points_off =
            RBuff.gemm_write_offset(layer, bacc_per_time[cur_time]);

        const auto hx_ptr = cur_time > 0 ? reserveSpace : hx;

        const miopenStatus_t gemm_status = CallGemm(handle,
                                                    gemm_desc_hx,
                                                    hx_ptr,
                                                    hx_ptr_offset,
                                                    w,
                                                    WeiBuf.get_matrix_h_off(layer),
                                                    reserveSpace,
                                                    RB_layer_save_points_off,
                                                    GemmBackend_t::miopengemm);

        if(gemm_status != miopenStatusSuccess)
            MIOPEN_THROW("GEMM execution failure");
    };

    auto call_hidden_state_update = [&RBuff,
                                     &get_HxBuff_offset,
                                     &bacc_per_time,
                                     &in_n,
                                     &handle,
                                     &wDesc,
                                     reserveSpace,
                                     cx,
                                     max_batch,
                                     hidden_size](int layer_id, int time_id) {
        auto RB_layer_save_points_off =
            RBuff.layer_offset(layer_id) + RBuff.gemm_write_relative_offset(bacc_per_time[time_id]);

        auto is_seq_begin = time_id == 0;

        const int direction = 0;
        const int cur_batch = in_n.at(time_id), use_batch = in_n.at(time_id);

        const int hy_stride = RBuff.gemm_write_stride(), wei_len = RBuff.gemm_write_size(),
                  wei_stride = RBuff.gemm_write_size();

        const size_t cx_offset = get_HxBuff_offset(layer_id);

        const size_t i_offset = RB_layer_save_points_off + RBuff.get_gate_relative_offset(0),
                     f_offset = RB_layer_save_points_off + RBuff.get_gate_relative_offset(1),
                     o_offset = RB_layer_save_points_off + RBuff.get_gate_relative_offset(2),
                     c_offset = RB_layer_save_points_off + RBuff.get_gate_relative_offset(3);

        const size_t cell_offset   = RB_layer_save_points_off + RBuff.ct_relative_offset(),
                     hidden_offset = RB_layer_save_points_off + RBuff.ht_relative_offset();

        const size_t cell_offset_pre =
            (time_id == 0) ? 0
                           : RBuff.layer_offset(layer_id) +
                                 RBuff.gemm_write_relative_offset(bacc_per_time[time_id - 1]) +
                                 RBuff.ct_relative_offset();

        const size_t activ_cell_offset =
            RBuff.extra_save_point_offset(layer_id, bacc_per_time[time_id]);

        LSTMForwardHiddenStateUpdate(handle,
                                     wDesc.GetType(),
                                     false,
                                     is_seq_begin,
                                     direction,
                                     max_batch,
                                     cur_batch,
                                     use_batch,

                                     hidden_size,
                                     hy_stride,
                                     wei_len,
                                     wei_stride,
                                     cx,
                                     cx_offset,
                                     reserveSpace,
                                     i_offset,
                                     f_offset,
                                     o_offset,
                                     c_offset,
                                     cell_offset,
                                     cell_offset_pre,
                                     activ_cell_offset,
                                     hidden_offset);
    };

    auto call_hy_cy_update = [&RBuff,
                              &get_HxBuff_offset,
                              &bacc_per_time,
                              &in_n,
                              &handle,
                              &wDesc,
                              reserveSpace,
                              hy,
                              cy,
                              max_batch,
                              hidden_size,
                              seq_len](int layer_id) {
        if(hy != nullptr || (cy != nullptr))
        {
            auto hcy_layer_offset = get_HxBuff_offset(layer_id);

            const std::vector<size_t> hcy_src_stride{
                RBuff.layer_stride(), static_cast<size_t>(RBuff.gemm_write_stride()), 1};
            const std::vector<size_t> hcy_dst_stride{
                static_cast<size_t>(hidden_size * max_batch), static_cast<size_t>(hidden_size), 1};

            for(int time_i = seq_len - 1; time_i >= 0; time_i--)
            {
                auto copy_batch = (time_i == seq_len - 1) ? in_n.at(time_i)
                                                          : in_n.at(time_i) - in_n.at(time_i + 1);
                if(copy_batch > 0)
                {
                    auto batch_id_relative = in_n.at(time_i) - copy_batch;
                    auto batch_id_abs      = bacc_per_time[time_i] + batch_id_relative;

                    auto hcy_batch_offset = batch_id_relative * hidden_size;

                    auto src_batch_offset = RBuff.layer_offset(layer_id) +
                                            RBuff.gemm_write_relative_offset(batch_id_abs);

                    const std::vector<size_t> hcy_copy_size{
                        1, static_cast<size_t>(copy_batch), static_cast<size_t>(hidden_size)};

                    auto src_desc =
                        miopen::TensorDescriptor(wDesc.GetType(), hcy_copy_size, hcy_src_stride);
                    auto dst_desc =
                        miopen::TensorDescriptor(wDesc.GetType(), hcy_copy_size, hcy_dst_stride);

                    if(hy != nullptr)
                    {
                        CopyTensor(handle,
                                   src_desc,
                                   reserveSpace,
                                   dst_desc,
                                   hy,
                                   src_batch_offset + RBuff.ht_relative_offset(),
                                   hcy_layer_offset + hcy_batch_offset);
                    }

                    if(cy != nullptr)
                    {
                        CopyTensor(handle,
                                   src_desc,
                                   reserveSpace,
                                   dst_desc,
                                   cy,
                                   src_batch_offset + RBuff.ct_relative_offset(),
                                   hcy_layer_offset + hcy_batch_offset);
                    }
                }
            }
        }
    };

    auto call_sync_all_stream_pull_to_root_stream = [&stream_pull, root_stream_id]() {
        const miopen::HipEventPtr main_event = make_hip_fast_event();
        hipEventRecord(main_event.get(), stream_pull[root_stream_id]);

        for(int i = 0; i < stream_pull.size(); i++)
        {
            if(i != root_stream_id)
                hipStreamWaitEvent(stream_pull[i], main_event.get(), 0);
        }
    };

    if(seq_len == 0)
        return;

    const int try_chunks_cnt = 16;
    const int time_chunk_sz  = ((seq_len + try_chunks_cnt - 1) / try_chunks_cnt);
    const int chunks_cnt     = (seq_len + time_chunk_sz - 1) / time_chunk_sz;

    std::vector<int> layer_inx_cur_time(nLayers, 0);
    std::vector<int> layer_hx_cur_time(nLayers, 0);
    std::vector<int> layer_upd_cur_time(nLayers, 0);

    std::vector<std::vector<miopen::HipEventPtr>> layer_chunk_end_event;

    layer_chunk_end_event.resize(nLayers);
    for(int layer_id = 0; layer_id < nLayers; layer_id++)
    {
        layer_chunk_end_event[layer_id].resize(chunks_cnt);
        for(int chunk_id = 0; chunk_id < chunks_cnt; chunk_id++)
            layer_chunk_end_event[layer_id][chunk_id] = make_hip_fast_event();
    }

    std::vector<int> layer_stream_id(nLayers, 2);
    layer_stream_id[0] = 1;

    auto call_inx_next_chunk_preload = [&](int layer_id) {
        auto start_time = layer_inx_cur_time[layer_id];
        auto time_cnt   = std::min(time_chunk_sz, seq_len - start_time);

        call_x_gemm(layer_id, start_time, time_cnt);
        layer_inx_cur_time[layer_id] += time_chunk_sz;
    };

    auto call_hx_next_gemm = [&](int layer_id) {
        auto cur_time = layer_hx_cur_time[layer_id];
        if(cur_time < seq_len)
        {
            call_hx_gemm(layer_id, cur_time);
            layer_hx_cur_time[layer_id]++;
        }
    };

    auto call_next_hidden_state_update = [&](int layer_id) {
        auto cur_time = layer_upd_cur_time[layer_id];
        if(cur_time < seq_len)
        {
            call_hidden_state_update(layer_id, cur_time);
            layer_upd_cur_time[layer_id]++;
        }
    };

    auto call_next_chunk_compute = [&handle,
                                    &stream_pull,
                                    &layer_stream_id,
                                    &call_next_hidden_state_update,
                                    &call_hx_next_gemm,
                                    &call_inx_next_chunk_preload,
                                    &layer_upd_cur_time,
                                    &layer_chunk_end_event,
                                    time_chunk_sz,
                                    seq_len](int layer_id) {
        auto stream_id = layer_stream_id[layer_id];
        handle.SetStreamFromPool(stream_id);

        const int chunk_id   = layer_upd_cur_time[layer_id] / time_chunk_sz;
        const int chunk_time = std::min(time_chunk_sz, seq_len - chunk_id * time_chunk_sz);

        if(layer_id > 0 && layer_stream_id[layer_id - 1] != stream_id)
            hipStreamWaitEvent(
                stream_pull[stream_id], layer_chunk_end_event[layer_id - 1][chunk_id].get(), 0);

        if(!(layer_id == 0 && chunk_id == 1))
        {
            call_inx_next_chunk_preload(layer_id);
        }

        for(int time_id = 0; time_id < chunk_time; time_id++)
        {
            call_hx_next_gemm(layer_id);
            call_next_hidden_state_update(layer_id);
        }
        hipEventRecord(layer_chunk_end_event[layer_id][chunk_id].get(), stream_pull[stream_id]);
    };

    { // reserveSpace clean set 0
        const int fill_val = 0;
        // if(biasMode == 0u) req
        hipMemsetAsync(reserveSpace, fill_val, reserveSpaceSize, handle.GetStream());
    }

    // stage 0 bias and input preload
    // stage 0.2 first chunk compute and preload
    {
        call_sync_all_stream_pull_to_root_stream();
        const auto first_layer_id  = 0;
        const auto stream_id       = layer_stream_id[first_layer_id]; // 1
        const auto extra_stream_id = 2;

        handle.SetStreamFromPool(stream_id);

        if(biasMode != 0u)
            call_bias_add(first_layer_id);

        call_next_chunk_compute(first_layer_id);

        handle.SetStreamFromPool(extra_stream_id);

        if(biasMode != 0u)
            for(int layer_id = 1; layer_id < nLayers; layer_id++)
                call_bias_add(layer_id);

        call_inx_next_chunk_preload(first_layer_id);

        // sync first to second stream
        const miopen::HipEventPtr next_chunk_inx = make_hip_fast_event();
        hipEventRecord(next_chunk_inx.get(), stream_pull[extra_stream_id]);
        hipStreamWaitEvent(stream_pull[stream_id], next_chunk_inx.get(), 0);
    }

    for(int layer_id = 0; layer_id < nLayers; layer_id++)
    {

        const auto main_stream_id = 1;
        handle.SetStreamFromPool(main_stream_id);

        // check for wich stream was assigned this layer. If it differs from current - set stream
        // wait event
        if(layer_stream_id[layer_id] != main_stream_id)
        {
            auto chunk_id = layer_upd_cur_time[layer_id] / time_chunk_sz;
            if(chunk_id > 0)
                hipStreamWaitEvent(stream_pull[main_stream_id],
                                   layer_chunk_end_event[layer_id][chunk_id - 1].get(),
                                   0);

            layer_stream_id[layer_id] = main_stream_id;
        }

        const int start_chunk = layer_upd_cur_time[layer_id] / time_chunk_sz;

        const int extra_layer_max_chunks =
            start_chunk +
            ((layer_id + 1 < nLayers - 1) ? (chunks_cnt - start_chunk) / 2 : chunks_cnt);

        for(int chunk_id = start_chunk; chunk_id < chunks_cnt; chunk_id++)
        {

            call_next_chunk_compute(layer_id);

            int extra_compute_layer = layer_id + 1;
            for(; extra_compute_layer < nLayers; extra_compute_layer++)
            {
                auto extra_chunk_id = layer_upd_cur_time[extra_compute_layer] / time_chunk_sz;
                if(extra_chunk_id < extra_layer_max_chunks && extra_chunk_id <= chunk_id)
                    break;
            }

            if(extra_compute_layer < nLayers)
                call_next_chunk_compute(extra_compute_layer);
        }

        handle.SetStreamFromPool(main_stream_id);
        // update hy, cy
        call_hy_cy_update(layer_id);
    }

    handle.SetStreamFromPool(root_stream_id);
    hipStreamWaitEvent(
        stream_pull[root_stream_id], layer_chunk_end_event[nLayers - 1][chunks_cnt - 1].get(), 0);

    // output tensor copy
    {
        const std::vector<size_t> y_copy_size{
            1, static_cast<size_t>(total_batch_size), static_cast<size_t>(out_vec)};

        const std::vector<size_t> y_src_stride{
            RBuff.layer_stride(), static_cast<size_t>(RBuff.gemm_write_stride()), 1};

        const std::vector<size_t> y_dst_stride{
            static_cast<size_t>(out_vec * total_batch_size), static_cast<size_t>(out_vec), 1};

        auto src_desc   = miopen::TensorDescriptor(wDesc.GetType(), y_copy_size, y_src_stride);
        auto y_dst_desc = miopen::TensorDescriptor(wDesc.GetType(), y_copy_size, y_dst_stride);

        CopyTensor(
            handle, src_desc, reserveSpace, y_dst_desc, y, RBuff.ht_offset(nLayers - 1, 0), 0);
    }
#else
    (void)handle;
    (void)seq_array;
    (void)xDesc;
    (void)x;
    (void)hxDesc;
    (void)hx;
    (void)cx;
    (void)wDesc;
    (void)w;
    (void)yDesc;
    (void)y;
    (void)hy;
    (void)cy;
    (void)reserveSpace;
    (void)reserveSpaceSize;

    MIOPEN_THROW("GEMM is not supported");
#endif
}

// Assuming sequence length is set to > 0 otherwise throw exception.
void RNNDescriptor::RNNForwardInference(Handle& handle,
                                        const int seqLen,
                                        c_array_view<const miopenTensorDescriptor_t> xDesc,
                                        ConstData_t x,
                                        const TensorDescriptor& hxDesc,
                                        ConstData_t hx,
                                        const TensorDescriptor& cxDesc,
                                        ConstData_t cx,
                                        const TensorDescriptor& wDesc,
                                        ConstData_t w,
                                        c_array_view<const miopenTensorDescriptor_t> yDesc,
                                        Data_t y,
                                        const TensorDescriptor& hyDesc,
                                        Data_t hy,
                                        const TensorDescriptor& cyDesc,
                                        Data_t cy,
                                        Data_t workSpace,
                                        size_t workSpaceSize) const
{

#if MIOPEN_USE_GEMM

    float ctime = 0.;
    // reset kernel timer
    profileRNNkernels(handle, 0, ctime);
    if(x == nullptr || w == nullptr || y == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(hxDesc.GetSize() != cxDesc.GetSize() || hxDesc.GetSize() != hyDesc.GetSize() ||
       hxDesc.GetSize() != cyDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(workSpaceSize < GetWorkspaceSize(handle, seqLen, xDesc))
    {
        MIOPEN_THROW("Workspace is required");
    }

    std::vector<int> in_n;
    int in_h  = xDesc[0].GetLengths()[1]; // input vector size
    int hy_d  = hyDesc.GetLengths()[0];   // biNumLayers
    int hy_n  = hyDesc.GetLengths()[1];   // max batch size
    int hy_h  = hyDesc.GetLengths()[2];   // hidden size
    int out_h = yDesc[0].GetLengths()[1]; // output vector size
    int bi    = dirMode != 0u ? 2 : 1;

    if(in_h <= 0 || hy_h <= 0 || hy_n <= 0 || hy_d <= 0 || out_h <= 0 || seqLen <= 0)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    if(out_h != (bi * hy_h))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Output size doesn't match hidden state size!");
    }

    if(inputMode == miopenRNNskip)
    {
        if(in_h != hy_h)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "The input tensor size must equal to the hidden "
                         "state size of the network in SKIP_INPUT mode!");
        }
        in_h = 0;
    }

    int batch_n = 0;
    for(int i = 0; i < seqLen; i++)
    {
        int batchval, inputvec, batchvalout, outputvec;
        std::tie(batchval, inputvec)     = miopen::tien<2>(xDesc[i].GetLengths());
        std::tie(batchvalout, outputvec) = miopen::tien<2>(yDesc[i].GetLengths());
        if(batchval != batchvalout)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "Input batch length: " + std::to_string(batchval) +
                             ", Output batch length: " + std::to_string(batchvalout));
        }
        if(i == 0)
        {
            if(batchval <= 0)
            {
                MIOPEN_THROW(miopenStatusBadParm, "Input batch is ZERO!");
            }
        }
        else
        {
            if(batchval > in_n.back() || batchval < 0)
            {
                MIOPEN_THROW(miopenStatusBadParm,
                             "Incorrect input batch size at time " + std::to_string(i) +
                                 "! Batch size must not ascend!");
            }
        }
        in_n.push_back(batchval);
        batch_n += batchval;
    }
    // input check end

    int in_stride  = xDesc[0].GetLengths()[1];
    int hy_stride  = hy_h * bi * static_cast<int>(workspaceScale);
    int out_stride = out_h;
    int wei_stride = hy_h * bi * static_cast<int>(nHiddenTensorsPerLayer);
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    size_t wei_shift_bias = (in_h + hy_h + (bi * hy_h + hy_h) * (nLayers - 1)) * wei_stride;
    size_t offset;
    float alpha0, alpha1, beta_t;
    float alpha = 1, beta = 0;

    std::vector<int> sp_size(3, 1), sp_stride(3, 1), w_size(3, 1), w_stride(3, 1), x_size(3, 1),
        x_stride(3, 1), y_size(3, 1), y_stride(3, 1), hx_size(3, 1), hx_stride(3, 1);
    miopen::TensorDescriptor sp_desc, w_desc, x_desc, y_desc, hx_desc;

    sp_size[2]   = workSpaceSize / GetTypeSize(wDesc.GetType());
    sp_stride[0] = sp_size[2];
    sp_stride[1] = sp_size[2];
    sp_desc      = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
    SetTensor(handle, sp_desc, workSpace, &beta);
    // Update time
    profileRNNkernels(handle, 1, ctime);
    sp_stride[0] = batch_n * hy_stride;
    sp_stride[1] = hy_stride;
    sp_size[2]   = 1;
    w_stride[0]  = wei_stride;
    w_stride[1]  = wei_stride;
    x_stride[0]  = batch_n * in_stride;
    x_stride[1]  = in_stride;
    y_stride[0]  = batch_n * out_stride;
    y_stride[1]  = out_stride;
    if(hy != nullptr || (rnnMode == miopenLSTM && cy != nullptr))
    {
        hx_size[2]   = hy_d * hy_n * hy_h;
        hx_stride[0] = hx_size[2];
        hx_stride[1] = hx_size[2];
        hx_desc      = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
        if(hy != nullptr)
        {
            SetTensor(handle, hx_desc, hy, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
        if(rnnMode == miopenLSTM && cy != nullptr)
        {
            SetTensor(handle, hx_desc, cy, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
    }
    hx_stride[0] = in_n.at(0) * uni_stride;
    hx_stride[1] = uni_stride;

    int wei_shift, prelayer_shift;
    int wei_len = 0;
    int hid_off = 0;

    switch(rnnMode)
    {
    case miopenRNNRELU:
    case miopenRNNTANH:
        // printf("run rnn gpu inference \n");
        wei_len = hy_h;
        hid_off = 0;
        break;
    case miopenLSTM:
        // printf("run lstm gpu inference \n");
        wei_len = hy_h * 4;
        hid_off = bi * hy_h * 5;
        break;
    case miopenGRU:
        // printf("run gru gpu inference \n");
        wei_len = hy_h * 3;
        hid_off = bi * hy_h * 3;
        break;
    }

    ActivationDescriptor tanhDesc, sigDesc, activDesc;
    sigDesc  = {miopenActivationLOGISTIC, 1, 0, 1};
    tanhDesc = {miopenActivationTANH, 1, 1, 1};
    if(rnnMode == miopenRNNRELU)
    {
        activDesc = {miopenActivationRELU, 1, 0, 1};
    }
    else if(rnnMode == miopenRNNTANH)
    {
        activDesc = {miopenActivationTANH, 1, 1, 1};
    }

    for(int li = 0; li < nLayers; li++)
    {
        int hid_shift           = li * batch_n * hy_stride;
        int hx_shift            = li * hy_n * bi_stride;
        int wei_shift_bias_temp = static_cast<int>(wei_shift_bias) + li * 2 * wei_stride;

        // from input
        if(li == 0)
        {
            if(inputMode == miopenRNNskip)
            {
                x_size[1]  = batch_n;
                x_size[2]  = hy_h;
                sp_size[1] = batch_n;
                sp_size[2] = hy_h;
                x_desc     = miopen::TensorDescriptor(wDesc.GetType(), x_size, x_stride);
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                for(int gi = 0; gi < nHiddenTensorsPerLayer * bi; gi++)
                {
                    CopyTensor(handle, x_desc, x, sp_desc, workSpace, 0, gi * hy_h);
                    // Update time
                    profileRNNkernels(handle, 1, ctime);
                }
            }
            else
            {
                miopen::GemmDescriptor gemm_desc =
                    GemmDescriptor{false,
                                   false,
                                   true,
                                   batch_n,
                                   wei_len * bi,
                                   in_h,
                                   in_stride,
                                   in_stride,
                                   hy_stride,
                                   1, // batch count
                                   0, // Stride A
                                   0, // Stride B
                                   0, // Stride C
                                   1, // alpha
                                   1, // beta
                                   xDesc[0].GetType(),
                                   false}; // RNN does not support determinism

                miopenStatus_t gemm_status = CallGemm(
                    handle, gemm_desc, x, 0, w, 0, workSpace, hid_shift, GemmBackend_t::miopengemm);

                if(gemm_status != miopenStatusSuccess)
                {
                    if(gemm_status == miopenStatusNotImplemented)
                    {
                        MIOPEN_LOG_E("GEMM not implemented");
                    }
                    else
                    {
                        MIOPEN_LOG_E("GEMM failed");
                    }
                }
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }
        else
        {
            wei_shift = (in_h + hy_h) * wei_stride + (li - 1) * (bi * hy_h + hy_h) * wei_stride;
            prelayer_shift = (li - 1) * batch_n * hy_stride + hid_off;

            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                              false,
                                                              true,
                                                              batch_n,
                                                              wei_len * bi,
                                                              hy_h * bi,
                                                              hy_stride,
                                                              bi_stride,
                                                              hy_stride,
                                                              1, // batch count
                                                              0, // Stride A
                                                              0, // Stride B
                                                              0, // Stride C
                                                              1, // alpha
                                                              1, // beta
                                                              xDesc[0].GetType(),
                                                              false};
            miopenStatus_t gemm_status       = CallGemm(handle,
                                                  gemm_desc,
                                                  workSpace,
                                                  prelayer_shift,
                                                  w,
                                                  wei_shift,
                                                  workSpace,
                                                  hid_shift,
                                                  GemmBackend_t::miopengemm);

            if(gemm_status != miopenStatusSuccess)
            {
                if(gemm_status == miopenStatusNotImplemented)
                {
                    MIOPEN_LOG_E("GEMM not implemented");
                }
                else
                {
                    MIOPEN_LOG_E("GEMM failed");
                }
            }
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        if(biasMode != 0u)
        {
            alpha0 = 1;
            alpha1 = 1;
            beta_t = 0;

            w_size[1]  = 1;
            w_size[2]  = wei_stride;
            sp_size[1] = batch_n;
            sp_size[2] = wei_stride;
            w_desc     = miopen::TensorDescriptor(wDesc.GetType(), w_size, w_stride);
            sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

            OpTensor(handle,
                     miopenTensorOpAdd,
                     &alpha0,
                     sp_desc,
                     workSpace,
                     &alpha1,
                     w_desc,
                     w,
                     &beta_t,
                     sp_desc,
                     workSpace,
                     hid_shift,
                     wei_shift_bias_temp,
                     hid_shift);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        if(rnnMode == miopenGRU)
        {
            sp_size[1] = batch_n;
            sp_size[2] = hy_h;
            sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

            alpha0 = 0;
            alpha1 = 0;
            beta_t = 0;
            for(int bs = 0; bs < bi; bs++)
            {
                CopyTensor(handle,
                           sp_desc,
                           workSpace,
                           sp_desc,
                           workSpace,
                           hid_shift + bs * wei_len + 2 * hy_h,
                           hid_shift + hid_off + bs * hy_h);
                // Update time
                profileRNNkernels(handle, 1, ctime);

                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         workSpace,
                         &alpha1,
                         sp_desc,
                         workSpace,
                         &beta_t,
                         sp_desc,
                         workSpace,
                         hid_shift + bs * wei_len + 2 * hy_h,
                         hid_shift + bs * wei_len + 2 * hy_h,
                         hid_shift + bs * wei_len + 2 * hy_h);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }

        if(biasMode != 0u)
        {
            wei_shift_bias_temp += wei_stride;

            alpha0 = 1;
            alpha1 = 1;
            beta_t = 0;

            if(hx != nullptr)
            {
                sp_size[1] = batch_n;
                sp_size[2] = wei_stride;
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         workSpace,
                         &alpha1,
                         w_desc,
                         w,
                         &beta_t,
                         sp_desc,
                         workSpace,
                         hid_shift,
                         wei_shift_bias_temp,
                         hid_shift);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
            else
            {
                sp_size[1] = batch_n - in_n.at(0);
                sp_size[2] = wei_len;
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                w_size[1]  = 1;
                w_size[2]  = wei_len;
                w_desc     = miopen::TensorDescriptor(wDesc.GetType(), w_size, w_stride);

                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         workSpace,
                         &alpha1,
                         w_desc,
                         w,
                         &beta_t,
                         sp_desc,
                         workSpace,
                         hid_shift + in_n.at(0) * hy_stride,
                         wei_shift_bias_temp,
                         hid_shift + in_n.at(0) * hy_stride);
                // Update time
                profileRNNkernels(handle, 1, ctime);

                if(dirMode != 0u)
                {
                    if(in_n.at(0) == in_n.at(seqLen - 1))
                    {
                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 w_desc,
                                 w,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 hid_shift + wei_len,
                                 wei_shift_bias_temp + wei_len,
                                 hid_shift + wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else
                    {
                        int cur_batch = 0;
                        for(int ti = 0; ti < seqLen; ti++)
                        {
                            if(ti != (seqLen - 1))
                            {
                                offset = hid_shift + cur_batch * hy_stride;

                                sp_size[1] = in_n.at(ti + 1);
                                sp_size[2] = wei_len;
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpAdd,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         w_desc,
                                         w,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + wei_len,
                                         wei_shift_bias_temp + wei_len,
                                         offset + wei_len);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                            cur_batch += in_n.at(ti);
                        }
                    }
                }
            }
        }

        // from hidden state
        int bacc   = 0;
        int baccbi = batch_n;
        for(int ti = 0; ti < seqLen; ti++)
        {
            baccbi -= in_n.at(seqLen - 1 - ti);
            wei_shift         = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;
            int pretime_shift = 0;
            int use_time      = 0;

            for(int ri = 0; ri < bi; ri++)
            {
                int cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                int cur_batch = ri == 0 ? bacc : baccbi;
                offset        = hid_shift + cur_batch * hy_stride;
                if(ti > 0)
                {
                    pretime_shift =
                        ri == 0 ? hid_shift + (bacc - in_n.at(ti - 1)) * hy_stride
                                : hid_shift + (baccbi + in_n.at(seqLen - 1 - ti)) * hy_stride;
                    use_time = ri == 0 ? ti : seqLen - ti;
                }

                if(in_n.at(cur_time) > 0)
                {
                    if(ti == 0)
                    {
                        if(hx != nullptr)
                        {
                            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                              false,
                                                                              true,
                                                                              in_n.at(cur_time),
                                                                              wei_len,
                                                                              hy_h,
                                                                              uni_stride,
                                                                              uni_stride,
                                                                              hy_stride,
                                                                              1, // batch count
                                                                              0, // Stride A
                                                                              0, // Stride B
                                                                              0, // Stride C
                                                                              1, // alpha
                                                                              1, // beta
                                                                              xDesc[0].GetType(),
                                                                              false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         hx,
                                         hx_shift + ri * hy_n * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         workSpace,
                                         static_cast<int>(offset) + ri * wei_len,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                    else
                    {
                        if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                        {
                            miopen::GemmDescriptor gemm_desc =
                                GemmDescriptor{false,
                                               false,
                                               true,
                                               (in_n.at(cur_time) - in_n.at(use_time)),
                                               wei_len,
                                               hy_h,
                                               uni_stride,
                                               uni_stride,
                                               hy_stride,
                                               1, // batch count
                                               0, // Stride A
                                               0, // Stride B
                                               0, // Stride C
                                               1, // alpha
                                               1, // beta
                                               xDesc[0].GetType(),
                                               false};
                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         hx,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         workSpace,
                                         static_cast<int>(offset) + ri * wei_len +
                                             in_n.at(use_time) * hy_stride,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        if(in_n.at(use_time) > 0)
                        {
                            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                              false,
                                                                              true,
                                                                              in_n.at(use_time),
                                                                              wei_len,
                                                                              hy_h,
                                                                              hy_stride,
                                                                              uni_stride,
                                                                              hy_stride,
                                                                              1, // batch count
                                                                              0, // Stride A
                                                                              0, // Stride B
                                                                              0, // Stride C
                                                                              1, // alpha
                                                                              1, // beta
                                                                              xDesc[0].GetType(),
                                                                              false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         workSpace,
                                         pretime_shift + hid_off + ri * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         workSpace,
                                         static_cast<int>(offset) + ri * wei_len,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }

                    // update hidden status
                    sp_size[1] = in_n.at(cur_time);
                    if(rnnMode == miopenRNNRELU || rnnMode == miopenRNNTANH)
                    {
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        activDesc.Forward(handle,
                                          &alpha,
                                          sp_desc,
                                          workSpace,
                                          &beta,
                                          sp_desc,
                                          workSpace,
                                          offset + static_cast<size_t>(ri) * wei_len,
                                          offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenLSTM)
                    {
                        if(algoMode == miopenRNNdefault)
                        {
                            LSTMForwardHiddenStateUpdate(
                                handle,
                                wDesc.GetType(),
                                true,
                                ti == 0,
                                ri,
                                in_n.at(0),
                                in_n.at(cur_time),
                                in_n.at(use_time),
                                hy_h,
                                hy_stride,
                                wei_len,
                                wei_stride,
                                cx,
                                hx_shift + ri * hy_n * hy_h,
                                workSpace,
                                offset + static_cast<size_t>(ri) * wei_len,
                                offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                offset + 2 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + 3 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + static_cast<size_t>(bi) * wei_len +
                                    static_cast<size_t>(ri) * hy_h,
                                pretime_shift + static_cast<size_t>(bi) * wei_len +
                                    static_cast<size_t>(ri) * hy_h,
                                0,
                                offset + hid_off + static_cast<size_t>(ri) * hy_h);

                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                            continue;
                        }

                        // active gate i, f, o
                        sp_size[2] = hy_h * 3;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        sigDesc.Forward(handle,
                                        &alpha,
                                        sp_desc,
                                        workSpace,
                                        &beta,
                                        sp_desc,
                                        workSpace,
                                        offset + static_cast<size_t>(ri) * wei_len,
                                        offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // active gate c
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         workSpace,
                                         &beta,
                                         sp_desc,
                                         workSpace,
                                         offset + 3 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len,
                                         offset + 3 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update cell state
                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 1;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + static_cast<size_t>(ri) * wei_len,
                                 offset + 3 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        if(ti == 0)
                        {
                            if(cx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && cx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time) > 0)
                            {
                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(use_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                         pretime_shift + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(cur_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }
                            }
                        }

                        // active cell state
                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         workSpace,
                                         &beta,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update hidden state
                        beta_t = 0;
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenGRU)
                    {
                        // active z, r gate
                        sp_size[2] = 2 * hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        sigDesc.Forward(handle,
                                        &alpha,
                                        sp_desc,
                                        workSpace,
                                        &beta,
                                        sp_desc,
                                        workSpace,
                                        offset + static_cast<size_t>(ri) * wei_len,
                                        offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // calculate c gate
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // active c gate
                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         workSpace,
                                         &beta,
                                         sp_desc,
                                         workSpace,
                                         offset + 2 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len,
                                         offset + 2 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // calculate hidden state
                        alpha0 = -1;
                        alpha1 = 1;
                        beta_t = 0;
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + static_cast<size_t>(ri) * wei_len,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 1;
                        if(ti == 0)
                        {
                            if(hx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         hx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         hx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time) > 0)
                            {
                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(use_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         pretime_shift + hid_off + ri * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                    }
                }
            }

            bacc += in_n.at(ti);
        }

        // update hy, cy
        if(hy != nullptr || (rnnMode == miopenLSTM && cy != nullptr))
        {
            hx_size[2] = hy_h;
            sp_size[2] = hy_h;

            bacc   = batch_n;
            baccbi = 0;
            for(int ti = seqLen - 1; ti >= 0; ti--)
            {
                bacc -= in_n.at(ti);
                for(int ri = 0; ri < bi; ri++)
                {
                    int cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                    int cur_batch = ri == 0 ? bacc : baccbi;
                    int use_batch = 0;

                    if(ti < seqLen - 1)
                    {
                        int use_time = ri == 0 ? ti + 1 : seqLen - 2 - ti;
                        use_batch    = in_n.at(use_time);
                    }

                    if(in_n.at(cur_time) > use_batch)
                    {
                        offset = hid_shift + cur_batch * hy_stride;

                        sp_size[1] = in_n.at(cur_time) - use_batch;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        hx_size[1] = sp_size[1];
                        hx_desc    = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                        if(hy != nullptr)
                        {
                            CopyTensor(handle,
                                       sp_desc,
                                       workSpace,
                                       hx_desc,
                                       hy,
                                       static_cast<int>(offset) + hid_off + ri * hy_h +
                                           use_batch * hy_stride,
                                       hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        if(rnnMode == miopenLSTM && cy != nullptr)
                        {
                            CopyTensor(handle,
                                       sp_desc,
                                       workSpace,
                                       hx_desc,
                                       cy,
                                       static_cast<int>(offset) + bi * wei_len + ri * hy_h +
                                           use_batch * hy_stride,
                                       hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                }
                baccbi += in_n.at(seqLen - 1 - ti);
            }
        }
    }

    // output
    prelayer_shift = (static_cast<int>(nLayers) - 1) * batch_n * hy_stride + hid_off;

    sp_size[1] = batch_n;
    sp_size[2] = hy_h * bi;
    y_size[1]  = batch_n;
    y_size[2]  = out_h;
    y_desc     = miopen::TensorDescriptor(wDesc.GetType(), y_size, y_stride);
    sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

    CopyTensor(handle, sp_desc, workSpace, y_desc, y, prelayer_shift, 0);
    // Update time
    profileRNNkernels(handle, 2, ctime);

#else
    (void)hx;
    (void)cx;
    (void)handle;
    (void)seqLen;
    (void)xDesc;
    (void)x;
    (void)w;
    (void)y;
    (void)hyDesc;
    (void)hy;
    (void)yDesc;
    (void)cyDesc;
    (void)cy;
    (void)hxDesc;
    (void)cxDesc;
    (void)wDesc;
    (void)workSpaceSize;
    (void)workSpace;
    MIOPEN_THROW("GEMM is not supported");
#endif
}

void RNNDescriptor::RNNForwardTraining(Handle& handle,
                                       const int seqLen,
                                       c_array_view<const miopenTensorDescriptor_t> xDesc,
                                       ConstData_t x,
                                       const TensorDescriptor& hxDesc,
                                       ConstData_t hx,
                                       const TensorDescriptor& cxDesc,
                                       ConstData_t cx,
                                       const TensorDescriptor& wDesc,
                                       ConstData_t w,
                                       c_array_view<const miopenTensorDescriptor_t> yDesc,
                                       Data_t y,
                                       const TensorDescriptor& hyDesc,
                                       Data_t hy,
                                       const TensorDescriptor& cyDesc,
                                       Data_t cy,
                                       Data_t workSpace,
                                       size_t workSpaceSize,
                                       Data_t reserveSpace,
                                       size_t reserveSpaceSize) const
{
    (void)workSpace;

#if MIOPEN_USE_GEMM

#if MIOPEN_BACKEND_HIP
    HipEventPtr start = nullptr;
    HipEventPtr stop  = nullptr;
    bool is_profiling = handle.IsProfilingEnabled();

    if(is_profiling)
    {
        handle.EnableProfiling(false);
        RNNProfilingBegin(handle, start, stop);
    }
#endif

    // OCL legacy
    float ctime = 0.;
    // reset kernel timer
    profileRNNkernels(handle, 0, ctime);

    if(x == nullptr || w == nullptr || y == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(hxDesc.GetSize() != cxDesc.GetSize() || hxDesc.GetSize() != hyDesc.GetSize() ||
       hxDesc.GetSize() != cyDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(workSpaceSize < GetWorkspaceSize(handle, seqLen, xDesc))
    {
        MIOPEN_THROW("Workspace is required");
    }
    if(reserveSpaceSize < GetReserveSize(handle, seqLen, xDesc))
    {
        MIOPEN_THROW("Reservespace is required");
    }

    int in_h  = xDesc[0].GetLengths()[1]; // input vector size
    int hy_d  = hyDesc.GetLengths()[0];   // biNumLayers
    int hy_n  = hyDesc.GetLengths()[1];   // max batch size
    int hy_h  = hyDesc.GetLengths()[2];   // hidden size
    int out_h = yDesc[0].GetLengths()[1]; // output vector size
    int bi    = dirMode != 0u ? 2 : 1;

    if(in_h <= 0 || hy_h <= 0 || hy_n <= 0 || hy_d <= 0 || out_h <= 0 || seqLen <= 0)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    if(out_h != (bi * hy_h))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Output size doesn't match hidden state size!");
    }

    if(inputMode == miopenRNNskip)
    {
        if(in_h != hy_h)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "The input tensor size must equal to the hidden "
                         "state size of the network in SKIP_INPUT mode!");
        }
        in_h = 0;
    }

    int batch_n = 0;
    std::vector<int> in_n;
    for(int i = 0; i < seqLen; i++)
    {
        int batchval, batchvalout;
        std::tie(batchval, std::ignore)    = miopen::tien<2>(xDesc[i].GetLengths());
        std::tie(batchvalout, std::ignore) = miopen::tien<2>(yDesc[i].GetLengths());
        if(batchval != batchvalout)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "Input batch length: " + std::to_string(batchval) +
                             ", Output batch length: " + std::to_string(batchvalout));
        }
        if(i == 0)
        {
            if(batchval <= 0)
            {
                MIOPEN_THROW(miopenStatusBadParm, "Input batch is ZERO!");
            }
        }
        else
        {
            if(batchval > in_n.back() || batchval < 0)
            {
                MIOPEN_THROW(miopenStatusBadParm,
                             "Incorrect input batch size at time " + std::to_string(i) +
                                 "! Batch size must not ascend!");
            }
        }
        in_n.push_back(batchval);
        batch_n += batchval;
    }
    // input check end
    bool use_dropout = !float_equal(miopen::deref(dropoutDesc).dropout, 0);
#if MIOPEN_USE_GEMM && MIOPEN_BACKEND_HIP

    if(rnnMode == miopenLSTM && algoMode == miopenRNNdefault && !use_dropout && nLayers > 1 &&
       dirMode == miopenRNNunidirection && inputMode != miopenRNNskip &&
       !(miopen::IsDisabled(MIOPEN_RNNFWD_exp{})) && xDesc[0].GetType() == miopenFloat &&
       seqLen >= 32)
    {
        RNNForwardTraining_MS(handle,
                              in_n,
                              xDesc[0],
                              x,
                              hxDesc,
                              hx,
                              cx,
                              wDesc,
                              w,
                              yDesc[0],
                              y,
                              hy,
                              cy,
                              reserveSpace,
                              reserveSpaceSize);

        if(is_profiling)
        {
            float eventTime_mS = RNNProfilingEnd(handle, start, stop);
            handle.EnableProfiling(true);
            handle.ResetKernelTime();
            handle.AccumKernelTime(eventTime_mS);
        }
        return;
    }
#endif // MIOPEN_USE_GEMM&& MIOPEN_BACKEND_HIP

    int in_stride  = xDesc[0].GetLengths()[1];
    int hy_stride  = hy_h * bi * static_cast<int>(workspaceScale);
    int out_stride = out_h;
    int wei_stride = hy_h * bi * static_cast<int>(nHiddenTensorsPerLayer);
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    size_t wei_shift_bias = (in_h + hy_h + (bi * hy_h + hy_h) * (nLayers - 1)) * wei_stride;
    size_t offset;
    float alpha0, alpha1, beta_t;
    float alpha = 1, beta = 0;

    std::vector<int> sp_size(3, 1), sp_stride(3, 1), w_size(3, 1), w_stride(3, 1), x_size(3, 1),
        x_stride(3, 1), y_size(3, 1), y_stride(3, 1), hx_size(3, 1), hx_stride(3, 1);
    miopen::TensorDescriptor sp_desc, w_desc, x_desc, y_desc, hx_desc;

    sp_size[2]   = reserveSpaceSize / GetTypeSize(wDesc.GetType());
    sp_stride[0] = sp_size[2];
    sp_stride[1] = sp_size[2];
    sp_desc      = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
    SetTensor(handle, sp_desc, reserveSpace, &beta);
    // Update time
    profileRNNkernels(handle, 1, ctime);
    sp_stride[0] = batch_n * hy_stride;
    sp_stride[1] = hy_stride;
    sp_size[2]   = 1;
    w_stride[0]  = wei_stride;
    w_stride[1]  = wei_stride;
    x_stride[0]  = batch_n * in_stride;
    x_stride[1]  = in_stride;
    y_stride[0]  = batch_n * out_stride;
    y_stride[1]  = out_stride;
    if(hy != nullptr || (rnnMode == miopenLSTM && cy != nullptr))
    {
        hx_size[2]   = hy_d * hy_n * hy_h;
        hx_stride[0] = hx_size[2];
        hx_stride[1] = hx_size[2];
        hx_desc      = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
        if(hy != nullptr)
        {
            SetTensor(handle, hx_desc, hy, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
        if(rnnMode == miopenLSTM && cy != nullptr)
        {
            SetTensor(handle, hx_desc, cy, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
    }
    hx_stride[0] = in_n.at(0) * uni_stride;
    hx_stride[1] = uni_stride;

    int wei_shift, prelayer_shift;
    int wei_len = 0;
    int hid_off = 0;

    switch(rnnMode)
    {
    case miopenRNNRELU:
    case miopenRNNTANH:
        // printf("run rnn gpu fwd \n");
        wei_len = hy_h;
        hid_off = static_cast<int>(nLayers) * batch_n * hy_stride;
        break;
    case miopenLSTM:
        // printf("run lstm gpu fwd \n");
        wei_len = hy_h * 4;
        hid_off = bi * hy_h * 5;
        break;
    case miopenGRU:
        // printf("run gru gpu fwd \n");
        wei_len = hy_h * 3;
        hid_off = bi * hy_h * 3;
        break;
    }

    ActivationDescriptor tanhDesc, sigDesc, activDesc;
    sigDesc  = {miopenActivationLOGISTIC, 1, 0, 1};
    tanhDesc = {miopenActivationTANH, 1, 1, 1};
    if(rnnMode == miopenRNNRELU)
    {
        activDesc = {miopenActivationRELU, 1, 0, 1};
    }
    else if(rnnMode == miopenRNNTANH)
    {
        activDesc = {miopenActivationTANH, 1, 1, 1};
    }

    for(int li = 0; li < nLayers; li++)
    {
        int hid_shift           = li * batch_n * hy_stride;
        int hx_shift            = li * hy_n * bi_stride;
        int wei_shift_bias_temp = static_cast<int>(wei_shift_bias) + li * 2 * wei_stride;

        // from input
        if(li == 0)
        {
            if(inputMode == miopenRNNskip)
            {
                x_size[1]  = batch_n;
                x_size[2]  = hy_h;
                sp_size[1] = batch_n;
                sp_size[2] = hy_h;
                x_desc     = miopen::TensorDescriptor(wDesc.GetType(), x_size, x_stride);
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                for(int gi = 0; gi < nHiddenTensorsPerLayer * bi; gi++)
                {
                    CopyTensor(handle, x_desc, x, sp_desc, reserveSpace, 0, gi * hy_h);
                    // Update time
                    profileRNNkernels(handle, 1, ctime);
                }
            }
            else
            {
                miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                  false,
                                                                  true,
                                                                  batch_n,
                                                                  wei_len * bi,
                                                                  in_h,
                                                                  in_stride,
                                                                  in_stride,
                                                                  hy_stride,
                                                                  1, // batch count
                                                                  0, // Stride A
                                                                  0, // Stride B
                                                                  0, // Stride C
                                                                  1, // alpha
                                                                  1, // beta
                                                                  xDesc[0].GetType(),
                                                                  false};

                miopenStatus_t gemm_status = CallGemm(handle,
                                                      gemm_desc,
                                                      x,
                                                      0,
                                                      w,
                                                      0,
                                                      reserveSpace,
                                                      hid_shift,
                                                      GemmBackend_t::miopengemm);

                if(gemm_status != miopenStatusSuccess)
                {
                    if(gemm_status == miopenStatusNotImplemented)
                    {
                        MIOPEN_LOG_E("GEMM not implemented");
                    }
                    else
                    {
                        MIOPEN_LOG_E("GEMM failed");
                    }
                }
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }
        else
        {
            wei_shift = (in_h + hy_h) * wei_stride + (li - 1) * (bi * hy_h + hy_h) * wei_stride;
            prelayer_shift = (li - 1) * batch_n * hy_stride + hid_off;

            if(use_dropout)
            {
                std::vector<int> drop_size(2), drop_in_str(2, 1), drop_out_str(2, 1);
                drop_size[0]    = batch_n;
                drop_size[1]    = hy_h * bi;
                drop_in_str[0]  = hy_stride;
                drop_out_str[0] = hy_h * bi;

                auto drop_in_desc =
                    miopen::TensorDescriptor(wDesc.GetType(), drop_size, drop_in_str);
                auto drop_out_desc =
                    miopen::TensorDescriptor(wDesc.GetType(), drop_size, drop_out_str);

                size_t drop_rsv_size = drop_out_desc.GetElementSize();
                size_t drop_rsv_start =
                    algoMode == miopenRNNdefault && rnnMode == miopenLSTM
                        ? nLayers * batch_n * hy_stride + nLayers * batch_n * hy_h * bi
                        : 2 * nLayers * batch_n * hy_stride;

                size_t drop_in_offset = prelayer_shift;
                size_t drop_out_offset =
                    drop_rsv_start + (static_cast<size_t>(li) - 1) * batch_n * hy_h * bi;
                size_t drop_rsv_offset = (drop_rsv_start + (nLayers - 1) * batch_n * hy_h * bi) *
                                             (wDesc.GetType() == miopenFloat ? 4 : 2) +
                                         (li - 1) * drop_rsv_size;

                miopen::deref(dropoutDesc)
                    .DropoutForward(handle,
                                    drop_in_desc,
                                    drop_in_desc,
                                    reserveSpace,
                                    drop_out_desc,
                                    reserveSpace,
                                    reserveSpace,
                                    drop_rsv_size,
                                    drop_in_offset,
                                    drop_out_offset,
                                    drop_rsv_offset);
                // Update time
                profileRNNkernels(handle, 1, ctime);
                prelayer_shift = drop_out_offset;
            }

            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                              false,
                                                              true,
                                                              batch_n,
                                                              wei_len * bi,
                                                              hy_h * bi,
                                                              use_dropout ? hy_h * bi : hy_stride,
                                                              bi_stride,
                                                              hy_stride,
                                                              1, // batch count
                                                              0, // Stride A
                                                              0, // Stride B
                                                              0, // Stride C
                                                              1, // alpha
                                                              1, // beta
                                                              xDesc[0].GetType(),
                                                              false};

            miopenStatus_t gemm_status = CallGemm(handle,
                                                  gemm_desc,
                                                  reserveSpace,
                                                  prelayer_shift,
                                                  w,
                                                  wei_shift,
                                                  reserveSpace,
                                                  hid_shift,
                                                  GemmBackend_t::miopengemm);

            if(gemm_status != miopenStatusSuccess)
            {
                if(gemm_status == miopenStatusNotImplemented)
                {
                    MIOPEN_LOG_E("GEMM not implemented");
                }
                else
                {
                    MIOPEN_LOG_E("GEMM failed");
                }
            }
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        if(biasMode != 0u)
        {
            alpha0 = 1;
            alpha1 = 1;
            beta_t = 0;

            w_size[1]  = 1;
            w_size[2]  = wei_stride;
            sp_size[1] = batch_n;
            sp_size[2] = wei_stride;
            w_desc     = miopen::TensorDescriptor(wDesc.GetType(), w_size, w_stride);
            sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

            OpTensor(handle,
                     miopenTensorOpAdd,
                     &alpha0,
                     sp_desc,
                     reserveSpace,
                     &alpha1,
                     w_desc,
                     w,
                     &beta_t,
                     sp_desc,
                     reserveSpace,
                     hid_shift,
                     wei_shift_bias_temp,
                     hid_shift);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        if(rnnMode == miopenGRU)
        {
            sp_size[1] = batch_n;
            sp_size[2] = hy_h;
            sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

            alpha0 = 0;
            alpha1 = 0;
            beta_t = 0;
            for(int bs = 0; bs < bi; bs++)
            {
                CopyTensor(handle,
                           sp_desc,
                           reserveSpace,
                           sp_desc,
                           reserveSpace,
                           hid_shift + bs * wei_len + 2 * hy_h,
                           hid_shift + hid_off + bs * hy_h);
                // Update time
                profileRNNkernels(handle, 1, ctime);
                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         reserveSpace,
                         &alpha1,
                         sp_desc,
                         reserveSpace,
                         &beta_t,
                         sp_desc,
                         reserveSpace,
                         hid_shift + bs * wei_len + 2 * hy_h,
                         hid_shift + bs * wei_len + 2 * hy_h,
                         hid_shift + bs * wei_len + 2 * hy_h);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }

        if(biasMode != 0u)
        {
            wei_shift_bias_temp += wei_stride;

            alpha0 = 1;
            alpha1 = 1;
            beta_t = 0;

            if(hx != nullptr)
            {
                sp_size[1] = batch_n;
                sp_size[2] = wei_stride;
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         reserveSpace,
                         &alpha1,
                         w_desc,
                         w,
                         &beta_t,
                         sp_desc,
                         reserveSpace,
                         hid_shift,
                         wei_shift_bias_temp,
                         hid_shift);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
            else
            {
                sp_size[1] = batch_n - in_n.at(0);
                sp_size[2] = wei_len;
                sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                w_size[1]  = 1;
                w_size[2]  = wei_len;
                w_desc     = miopen::TensorDescriptor(wDesc.GetType(), w_size, w_stride);

                OpTensor(handle,
                         miopenTensorOpAdd,
                         &alpha0,
                         sp_desc,
                         reserveSpace,
                         &alpha1,
                         w_desc,
                         w,
                         &beta_t,
                         sp_desc,
                         reserveSpace,
                         hid_shift + in_n.at(0) * hy_stride,
                         wei_shift_bias_temp,
                         hid_shift + in_n.at(0) * hy_stride);
                // Update time
                profileRNNkernels(handle, 1, ctime);

                if(dirMode != 0u)
                {
                    if(in_n.at(0) == in_n.at(seqLen - 1))
                    {
                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 w_desc,
                                 w,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 hid_shift + wei_len,
                                 wei_shift_bias_temp + wei_len,
                                 hid_shift + wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else
                    {
                        int cur_batch = 0;
                        for(int ti = 0; ti < seqLen; ti++)
                        {
                            if(ti != (seqLen - 1))
                            {
                                offset = hid_shift + cur_batch * hy_stride;

                                sp_size[1] = in_n.at(ti + 1);
                                sp_size[2] = wei_len;
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpAdd,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         w_desc,
                                         w,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         static_cast<int>(offset) + wei_len,
                                         wei_shift_bias_temp + wei_len,
                                         static_cast<int>(offset) + wei_len);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                            cur_batch += in_n.at(ti);
                        }
                    }
                }
            }
        }

        // from hidden state
        int bacc   = 0;
        int baccbi = batch_n;
        for(int ti = 0; ti < seqLen; ti++)
        {
            baccbi -= in_n.at(seqLen - 1 - ti);
            wei_shift         = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;
            int pretime_shift = 0;
            int use_time      = 0;

            for(int ri = 0; ri < bi; ri++)
            {
                int cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                int cur_batch = ri == 0 ? bacc : baccbi;
                offset        = hid_shift + cur_batch * hy_stride;
                if(ti > 0)
                {
                    pretime_shift =
                        ri == 0 ? hid_shift + (bacc - in_n.at(ti - 1)) * hy_stride
                                : hid_shift + (baccbi + in_n.at(seqLen - 1 - ti)) * hy_stride;
                    use_time = ri == 0 ? ti : seqLen - ti;
                }

                if(in_n.at(cur_time) > 0)
                {
                    if(ti == 0)
                    {
                        if(hx != nullptr)
                        {
                            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                              false,
                                                                              true,
                                                                              in_n.at(cur_time),
                                                                              wei_len,
                                                                              hy_h,
                                                                              uni_stride,
                                                                              uni_stride,
                                                                              hy_stride,
                                                                              1, // batch count
                                                                              0, // Stride A
                                                                              0, // Stride B
                                                                              0, // Stride C
                                                                              1, // alpha
                                                                              1, // beta
                                                                              xDesc[0].GetType(),
                                                                              false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         hx,
                                         hx_shift + ri * hy_n * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         reserveSpace,
                                         static_cast<int>(offset) + ri * wei_len,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                    else
                    {
                        if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                        {
                            miopen::GemmDescriptor gemm_desc =
                                GemmDescriptor{false,
                                               false,
                                               true,
                                               (in_n.at(cur_time) - in_n.at(use_time)),
                                               wei_len,
                                               hy_h,
                                               uni_stride,
                                               uni_stride,
                                               hy_stride,
                                               1, // batch count
                                               0, // Stride A
                                               0, // Stride B
                                               0, // Stride C
                                               1, // alpha
                                               1, // beta
                                               xDesc[0].GetType(),
                                               false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         hx,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         reserveSpace,
                                         static_cast<int>(offset) + ri * wei_len +
                                             in_n.at(use_time) * hy_stride,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        if(in_n.at(use_time) > 0)
                        {
                            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                              false,
                                                                              true,
                                                                              in_n.at(use_time),
                                                                              wei_len,
                                                                              hy_h,
                                                                              hy_stride,
                                                                              uni_stride,
                                                                              hy_stride,
                                                                              1, // batch count
                                                                              0, // Stride A
                                                                              0, // Stride B
                                                                              0, // Stride C
                                                                              1, // alpha
                                                                              1, // beta
                                                                              xDesc[0].GetType(),
                                                                              false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         reserveSpace,
                                         pretime_shift + hid_off + ri * hy_h,
                                         w,
                                         wei_shift + ri * wei_len * uni_stride,
                                         reserveSpace,
                                         static_cast<int>(offset) + ri * wei_len,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }

                    // update hidden status
                    sp_size[1] = in_n.at(cur_time);
                    if(rnnMode == miopenRNNRELU || rnnMode == miopenRNNTANH)
                    {
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        activDesc.Forward(handle,
                                          &alpha,
                                          sp_desc,
                                          reserveSpace,
                                          &beta,
                                          sp_desc,
                                          reserveSpace,
                                          offset + static_cast<size_t>(ri) * wei_len,
                                          offset + static_cast<size_t>(ri) * wei_len +
                                              static_cast<size_t>(nLayers) * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenLSTM)
                    {
                        if(algoMode == miopenRNNdefault)
                        {
                            LSTMForwardHiddenStateUpdate(
                                handle,
                                wDesc.GetType(),
                                false,
                                ti == 0,
                                ri,
                                in_n.at(0),
                                in_n.at(cur_time),
                                in_n.at(use_time),
                                hy_h,
                                hy_stride,
                                wei_len,
                                wei_stride,
                                cx,
                                hx_shift + ri * hy_n * hy_h,
                                reserveSpace,
                                offset + static_cast<size_t>(ri) * wei_len,
                                offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                offset + 2 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + 3 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + static_cast<size_t>(bi) * wei_len +
                                    static_cast<size_t>(ri) * hy_h,
                                pretime_shift + static_cast<size_t>(bi) * wei_len +
                                    static_cast<size_t>(ri) * hy_h,
                                (li * batch_n + cur_batch) * bi * hy_h + ri * hy_h +
                                    nLayers * batch_n * hy_stride,
                                offset + hid_off + static_cast<size_t>(ri) * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                            continue;
                        }

                        // active gate i, f, o
                        sp_size[2] = hy_h * 3;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        sigDesc.Forward(handle,
                                        &alpha,
                                        sp_desc,
                                        reserveSpace,
                                        &beta,
                                        sp_desc,
                                        reserveSpace,
                                        offset + static_cast<size_t>(ri) * wei_len,
                                        offset + static_cast<size_t>(ri) * wei_len +
                                            static_cast<size_t>(nLayers) * batch_n * hy_stride);

                        // active gate c
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         reserveSpace,
                                         &beta,
                                         sp_desc,
                                         reserveSpace,
                                         offset + 3 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len,
                                         offset + 3 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len +
                                             nLayers * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update cell state
                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 1;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + 3 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        if(ti == 0)
                        {
                            if(cx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                             nLayers * batch_n * hy_stride,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && cx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride +
                                             nLayers * batch_n * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time) > 0)
                            {
                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(use_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         pretime_shift + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(cur_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }
                            }
                        }

                        // active cell state
                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         reserveSpace,
                                         &beta,
                                         sp_desc,
                                         reserveSpace,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             nLayers * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update hidden state
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenGRU)
                    {
                        // active z, r gate
                        sp_size[2] = 2 * hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        sigDesc.Forward(handle,
                                        &alpha,
                                        sp_desc,
                                        reserveSpace,
                                        &beta,
                                        sp_desc,
                                        reserveSpace,
                                        offset + static_cast<size_t>(ri) * wei_len,
                                        offset + static_cast<size_t>(ri) * wei_len +
                                            static_cast<size_t>(nLayers) * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // calculate c gate
                        sp_size[2] = hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        CopyTensor(handle,
                                   sp_desc,
                                   reserveSpace,
                                   sp_desc,
                                   reserveSpace,
                                   static_cast<int>(offset) + 2 * hy_h + ri * wei_len,
                                   static_cast<int>(offset) + hid_off + ri * hy_h +
                                       static_cast<int>(nLayers) * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // active c gate
                        tanhDesc.Forward(handle,
                                         &alpha,
                                         sp_desc,
                                         reserveSpace,
                                         &beta,
                                         sp_desc,
                                         reserveSpace,
                                         offset + 2 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len,
                                         offset + 2 * static_cast<size_t>(hy_h) +
                                             static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // calculate hidden state
                        alpha0 = -1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h,
                                 offset + hid_off + static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 1;

                        if(ti == 0)
                        {
                            if(hx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         hx_desc,
                                         hx,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         hx_desc,
                                         hx,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time) > 0)
                            {
                                if(in_n.at(use_time) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(use_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         pretime_shift + hid_off + ri * hy_h,
                                         offset + hid_off + static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                    }
                }
            }

            bacc += in_n.at(ti);
        }

        // update hy, cy
        if(hy != nullptr || (rnnMode == miopenLSTM && cy != nullptr))
        {
            hx_size[2] = hy_h;
            sp_size[2] = hy_h;

            bacc   = batch_n;
            baccbi = 0;
            for(int ti = seqLen - 1; ti >= 0; ti--)
            {
                bacc -= in_n.at(ti);
                for(int ri = 0; ri < bi; ri++)
                {
                    int cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                    int cur_batch = ri == 0 ? bacc : baccbi;
                    int use_batch = 0;

                    if(ti < seqLen - 1)
                    {
                        int use_time = ri == 0 ? ti + 1 : seqLen - 2 - ti;
                        use_batch    = in_n.at(use_time);
                    }

                    if(in_n.at(cur_time) > use_batch)
                    {
                        offset = hid_shift + cur_batch * hy_stride;

                        sp_size[1] = in_n.at(cur_time) - use_batch;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        hx_size[1] = sp_size[1];
                        hx_desc    = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                        if(hy != nullptr)
                        {
                            CopyTensor(handle,
                                       sp_desc,
                                       reserveSpace,
                                       hx_desc,
                                       hy,
                                       static_cast<int>(offset) + hid_off + ri * hy_h +
                                           use_batch * hy_stride,
                                       hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        if(rnnMode == miopenLSTM && cy != nullptr)
                        {
                            CopyTensor(handle,
                                       sp_desc,
                                       reserveSpace,
                                       hx_desc,
                                       cy,
                                       static_cast<int>(offset) + bi * wei_len + ri * hy_h +
                                           use_batch * hy_stride,
                                       hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                }
                baccbi += in_n.at(seqLen - 1 - ti);
            }
        }
    }

    // output
    prelayer_shift = (static_cast<int>(nLayers) - 1) * batch_n * hy_stride + hid_off;

    sp_size[1] = batch_n;
    sp_size[2] = hy_h * bi;
    y_size[1]  = batch_n;
    y_size[2]  = out_h;
    y_desc     = miopen::TensorDescriptor(wDesc.GetType(), y_size, y_stride);
    sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

    CopyTensor(handle, sp_desc, reserveSpace, y_desc, y, prelayer_shift, 0);
    // Update time
    profileRNNkernels(handle, 2, ctime);

#if MIOPEN_BACKEND_HIP
    if(is_profiling)
    {
        float eventTime_mS = RNNProfilingEnd(handle, start, stop);
        handle.EnableProfiling(true);
        handle.ResetKernelTime();
        handle.AccumKernelTime(eventTime_mS);
    }
#endif

#else
    (void)handle;
    (void)seqLen;
    (void)xDesc;
    (void)x;
    (void)w;
    (void)hx;
    (void)cx;
    (void)y;
    (void)hyDesc;
    (void)hy;
    (void)yDesc;
    (void)cyDesc;
    (void)cy;
    (void)hxDesc;
    (void)cxDesc;
    (void)wDesc;
    (void)workSpaceSize;
    (void)reserveSpace;
    (void)reserveSpaceSize;
    MIOPEN_THROW("GEMM is not supported");
#endif
};

void RNNDescriptor::RNNBackwardData(Handle& handle,
                                    const int seqLen,
                                    c_array_view<const miopenTensorDescriptor_t> yDesc,
                                    ConstData_t y,
                                    c_array_view<const miopenTensorDescriptor_t> dyDesc,
                                    ConstData_t dy,
                                    const TensorDescriptor& dhyDesc,
                                    ConstData_t dhy,
                                    const TensorDescriptor& dcyDesc,
                                    ConstData_t dcy,
                                    const TensorDescriptor& wDesc,
                                    ConstData_t w,
                                    const TensorDescriptor& hxDesc,
                                    ConstData_t hx,
                                    const TensorDescriptor& cxDesc,
                                    ConstData_t cx,
                                    c_array_view<const miopenTensorDescriptor_t> dxDesc,
                                    Data_t dx,
                                    const TensorDescriptor& dhxDesc,
                                    Data_t dhx,
                                    const TensorDescriptor& dcxDesc,
                                    Data_t dcx,
                                    Data_t workSpace,
                                    size_t workSpaceSize,
                                    Data_t reserveSpace,
                                    size_t reserveSpaceSize) const
{

    // Suppress warning
    (void)y;
    (void)yDesc;
    (void)hxDesc;
    (void)cxDesc;
    (void)dcxDesc;
    (void)dcyDesc;
    (void)dhyDesc;
    (void)wDesc;
#if MIOPEN_USE_GEMM

    float ctime = 0.;
    // reset kernel timer
    profileRNNkernels(handle, 0, ctime);

    if(dx == nullptr || w == nullptr || dy == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(dhyDesc.GetSize() != dcyDesc.GetSize() || dhyDesc.GetSize() != hxDesc.GetSize() ||
       dhyDesc.GetSize() != cxDesc.GetSize() || dhyDesc.GetSize() != dhxDesc.GetSize() ||
       dhyDesc.GetSize() != dcxDesc.GetSize())
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(workSpaceSize < GetWorkspaceSize(handle, seqLen, dxDesc))
    {
        MIOPEN_THROW("Workspace is required");
    }
    if(reserveSpaceSize < GetReserveSize(handle, seqLen, dxDesc))
    {
        MIOPEN_THROW("Reservespace is required");
    }

    std::vector<int> in_n;
    int in_h  = dxDesc[0].GetLengths()[1];
    int hy_d  = dhxDesc.GetLengths()[0];
    int hy_n  = dhxDesc.GetLengths()[1];
    int hy_h  = dhxDesc.GetLengths()[2];
    int out_h = dyDesc[0].GetLengths()[1];

    if(in_h <= 0 || hy_h <= 0 || hy_n <= 0 || hy_d <= 0 || out_h <= 0 || seqLen <= 0)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    int batch_n = 0;
    for(int i = 0; i < seqLen; i++)
    {
        int batchval, inputvec, batchvalout, outputvec;
        std::tie(batchval, inputvec)     = miopen::tien<2>(dxDesc[i].GetLengths());
        std::tie(batchvalout, outputvec) = miopen::tien<2>(dyDesc[i].GetLengths());
        if(batchval != batchvalout)
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }
        if(i == 0)
        {
            if(batchval <= 0)
            {
                MIOPEN_THROW(miopenStatusBadParm, "Input batch is ZERO!");
            }
        }
        else
        {
            if(batchval > in_n.back() || batchval < 0)
            {
                MIOPEN_THROW(miopenStatusBadParm,
                             "Incorrect input batch size at time " + std::to_string(i) +
                                 "! Batch size must not ascend!");
            }
        }
        in_n.push_back(batchval);
        batch_n += dxDesc[i].GetLengths()[0];
    }

    int bi = dirMode != 0u ? 2 : 1;
    if(out_h != (bi * hy_h))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Output size doesn't match hidden state size!");
    }

    int in_stride  = in_h;
    int hy_stride  = hy_h * bi * static_cast<int>(workspaceScale);
    int out_stride = out_h;
    int wei_stride = hy_h * bi * static_cast<int>(nHiddenTensorsPerLayer);
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    if(inputMode == miopenRNNskip)
    {
        if(in_h != hy_h)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "The input tensor size must equal to the hidden "
                         "state size of the network in SKIP_INPUT mode!");
        }
        in_h = 0;
    }

    size_t offset;
    float alpha0, alpha1, beta_t;
    float alpha = 1, beta = 0;

    std::vector<int> sp_size(3, 1), sp_stride(3, 1), x_size(3, 1), x_stride(3, 1), y_size(3, 1),
        y_stride(3, 1), hx_size(3, 1), hx_stride(3, 1);
    miopen::TensorDescriptor sp_desc, x_desc, y_desc, hx_desc;

    sp_size[2]   = workSpaceSize / GetTypeSize(wDesc.GetType());
    sp_stride[0] = sp_size[2];
    sp_stride[1] = sp_size[2];
    sp_desc      = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
    SetTensor(handle, sp_desc, workSpace, &beta);
    // Update time
    profileRNNkernels(handle, 1, ctime);
    sp_stride[0] = batch_n * hy_stride;
    sp_stride[1] = hy_stride;
    sp_size[2]   = 1;
    x_stride[0]  = batch_n * in_stride;
    x_stride[1]  = in_stride;
    y_stride[0]  = batch_n * out_stride;
    y_stride[1]  = out_stride;
    if(dhx != nullptr || (rnnMode == miopenLSTM && dcx != nullptr))
    {
        hx_size[2]   = hy_d * hy_n * hy_h;
        hx_stride[0] = hx_size[2];
        hx_stride[1] = hx_size[2];
        hx_desc      = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
        if(dhx != nullptr)
        {
            SetTensor(handle, hx_desc, dhx, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
        if(rnnMode == miopenLSTM && dcx != nullptr)
        {
            SetTensor(handle, hx_desc, dcx, &beta);
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }
    }
    hx_stride[0] = in_n.at(0) * uni_stride;
    hx_stride[1] = uni_stride;

    int prelayer_shift, pretime_shift, cur_time, cur_batch;
    int wei_len    = 0;
    int wei_len_t  = 0;
    int dhd_off    = 0;
    int use_time   = 0;
    int pre_batch  = 0;
    int use_time2  = 0;
    int pre_batch2 = 0;

    switch(rnnMode)
    {
    case miopenRNNRELU:
    case miopenRNNTANH:
        // printf("run rnn gpu bwd data \n");
        wei_len   = hy_h;
        wei_len_t = hy_h;
        dhd_off   = 0;
        break;
    case miopenLSTM:
        // printf("run lstm gpu bwd data \n");
        wei_len   = hy_h * 4;
        wei_len_t = hy_h * 4;
        dhd_off   = bi * hy_h * 5;
        break;
    case miopenGRU:
        // printf("run gru gpu bwd data \n");
        wei_len   = hy_h * 3;
        wei_len_t = hy_h * 2;
        dhd_off   = bi * hy_h * 3;
        break;
    }

    ActivationDescriptor tanhDesc, sigDesc, activDesc;
    sigDesc  = {miopenActivationLOGISTIC, 1, 0, 1};
    tanhDesc = {miopenActivationTANH, 1, 1, 1};
    if(rnnMode == miopenRNNRELU)
    {
        activDesc = {miopenActivationRELU, 1, 0, 1};
    }
    else if(rnnMode == miopenRNNTANH)
    {
        activDesc = {miopenActivationTANH, 1, 1, 1};
    }

    for(int li = static_cast<int>(nLayers) - 1; li >= 0; li--)
    {
        int wei_shift     = (in_h + hy_h) * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;
        int hid_shift     = li * batch_n * hy_stride;
        int hx_shift      = li * hy_n * bi_stride;
        int weitime_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

        // feedback from output
        if(li == nLayers - 1)
        {
            y_size[1]  = batch_n;
            y_size[2]  = out_h;
            sp_size[1] = batch_n;
            sp_size[2] = hy_h * bi;
            y_desc     = miopen::TensorDescriptor(wDesc.GetType(), y_size, y_stride);
            sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

            CopyTensor(handle, y_desc, dy, sp_desc, workSpace, 0, hid_shift + dhd_off);
            // Update time
            profileRNNkernels(handle, 1, ctime); // start timing
        }
        else
        {
            prelayer_shift                   = (li + 1) * batch_n * hy_stride;
            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                              false,
                                                              false,
                                                              batch_n,
                                                              hy_h * bi,
                                                              wei_len * bi,
                                                              hy_stride,
                                                              bi_stride,
                                                              hy_stride,
                                                              1, // batch count
                                                              0, // Stride A
                                                              0, // Stride B
                                                              0, // Stride C
                                                              1, // alpha
                                                              1, // beta
                                                              yDesc[0].GetType(),
                                                              false};

            miopenStatus_t gemm_status = CallGemm(handle,
                                                  gemm_desc,
                                                  workSpace,
                                                  prelayer_shift,
                                                  w,
                                                  wei_shift,
                                                  workSpace,
                                                  hid_shift + dhd_off,
                                                  GemmBackend_t::miopengemm);

            if(gemm_status != miopenStatusSuccess)
            {
                if(gemm_status == miopenStatusNotImplemented)
                {
                    MIOPEN_LOG_E("GEMM not implemented");
                }
                else
                {
                    MIOPEN_LOG_E("GEMM failed");
                }
            }
            // Update time
            profileRNNkernels(handle, 1, ctime);

            if(!float_equal(miopen::deref(dropoutDesc).dropout, 0))
            {
                std::vector<int> drop_size(2), drop_in_str(2, 1);
                drop_size[0]   = batch_n;
                drop_size[1]   = hy_h * bi;
                drop_in_str[0] = hy_stride;

                auto drop_in_desc =
                    miopen::TensorDescriptor(wDesc.GetType(), drop_size, drop_in_str);

                size_t drop_rsv_size = drop_in_desc.GetElementSize();
                size_t drop_rsv_start =
                    algoMode == miopenRNNdefault && rnnMode == miopenLSTM
                        ? nLayers * batch_n * hy_stride + nLayers * batch_n * hy_h * bi
                        : 2 * nLayers * batch_n * hy_stride;

                size_t drop_rsv_offset = (drop_rsv_start + (nLayers - 1) * batch_n * hy_h * bi) *
                                             (wDesc.GetType() == miopenFloat ? 4 : 2) +
                                         li * drop_rsv_size;

                miopen::deref(dropoutDesc)
                    .DropoutBackward(handle,
                                     drop_in_desc,
                                     drop_in_desc,
                                     workSpace,
                                     drop_in_desc,
                                     workSpace,
                                     reserveSpace,
                                     drop_rsv_size,
                                     hid_shift + dhd_off,
                                     hid_shift + dhd_off,
                                     drop_rsv_offset);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }

        // from hidden state
        int bacc   = batch_n;
        int baccbi = 0;
        for(int ti = seqLen - 1; ti >= 0; ti--)
        {
            bacc -= in_n.at(ti);

            // from post state
            for(int ri = 0; ri < bi; ri++)
            {
                cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                cur_batch = ri == 0 ? bacc : baccbi;
                offset    = hid_shift + cur_batch * hy_stride;
                if(ti < seqLen - 1)
                {
                    use_time  = ri == 0 ? ti + 1 : seqLen - 1 - ti;
                    pre_batch = ri == 0 ? bacc + in_n.at(ti) : baccbi - in_n.at(seqLen - 2 - ti);
                }
                if(ti > 0)
                {
                    use_time2 = ri == 0 ? ti : seqLen - ti;
                    pre_batch2 =
                        ri == 0 ? bacc - in_n.at(ti - 1) : baccbi + in_n.at(seqLen - 1 - ti);
                }

                if(in_n.at(cur_time) > 0)
                {
                    if(ti == seqLen - 1)
                    {
                        if(dhy != nullptr)
                        {
                            alpha0 = 1;
                            alpha1 = 1;
                            beta_t = 0;

                            hx_size[1] = in_n.at(cur_time);
                            hx_size[2] = hy_h;
                            sp_size[1] = in_n.at(cur_time);
                            sp_size[2] = hy_h;
                            hx_desc = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                            sp_desc = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                            OpTensor(handle,
                                     miopenTensorOpAdd,
                                     &alpha0,
                                     hx_desc,
                                     dhy,
                                     &alpha1,
                                     sp_desc,
                                     workSpace,
                                     &beta_t,
                                     sp_desc,
                                     workSpace,
                                     hx_shift + ri * hy_n * hy_h,
                                     offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                     offset + dhd_off + static_cast<size_t>(ri) * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                    else
                    {
                        if(ri == 0 && dhy != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                        {
                            alpha0 = 1;
                            alpha1 = 1;
                            beta_t = 0;

                            hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                            hx_size[2] = hy_h;
                            sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                            sp_size[2] = hy_h;
                            hx_desc = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                            sp_desc = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                            OpTensor(handle,
                                     miopenTensorOpAdd,
                                     &alpha0,
                                     hx_desc,
                                     dhy,
                                     &alpha1,
                                     sp_desc,
                                     workSpace,
                                     &beta_t,
                                     sp_desc,
                                     workSpace,
                                     hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                     offset + dhd_off + static_cast<size_t>(ri) * hy_h +
                                         static_cast<size_t>(in_n.at(use_time)) * hy_stride,
                                     offset + dhd_off + static_cast<size_t>(ri) * hy_h +
                                         static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        pretime_shift =
                            li * batch_n * hy_stride + pre_batch * hy_stride + ri * wei_len;

                        if(in_n.at(use_time) > 0)
                        {
                            if(rnnMode == miopenGRU)
                            {
                                sp_size[1] = in_n.at(use_time);
                                sp_size[2] = hy_h;
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                alpha0 = 1;
                                alpha1 = 1;
                                beta_t = 1;

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         pretime_shift - ri * 2 * hy_h + dhd_off,
                                         pretime_shift + nLayers * batch_n * hy_stride,
                                         offset + dhd_off + static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                CopyTensor(handle,
                                           sp_desc,
                                           workSpace,
                                           sp_desc,
                                           workSpace,
                                           pretime_shift + 2 * hy_h,
                                           static_cast<int>(offset) + ri * wei_len + 2 * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                CopyTensor(handle,
                                           sp_desc,
                                           reserveSpace,
                                           sp_desc,
                                           workSpace,
                                           pretime_shift - ri * 2 * hy_h + dhd_off +
                                               static_cast<int>(nLayers) * batch_n * hy_stride,
                                           pretime_shift + 2 * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                              false,
                                                                              false,
                                                                              in_n.at(use_time),
                                                                              hy_h,
                                                                              wei_len,
                                                                              hy_stride,
                                                                              uni_stride,
                                                                              hy_stride,
                                                                              1, // batch count
                                                                              0, // Stride A
                                                                              0, // Stride B
                                                                              0, // Stride C
                                                                              1, // alpha
                                                                              1, // beta
                                                                              yDesc[0].GetType(),
                                                                              false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         workSpace,
                                         pretime_shift,
                                         w,
                                         weitime_shift + ri * wei_len * uni_stride,
                                         workSpace,
                                         static_cast<int>(offset) + dhd_off + ri * hy_h,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);

                            if(rnnMode == miopenGRU)
                            {
                                CopyTensor(handle,
                                           sp_desc,
                                           workSpace,
                                           sp_desc,
                                           workSpace,
                                           static_cast<int>(offset) + ri * wei_len + 2 * hy_h,
                                           pretime_shift + 2 * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                    }

                    // update hidden status
                    sp_size[1] = in_n.at(cur_time);
                    sp_size[2] = hy_h;
                    sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                    if(rnnMode == miopenRNNRELU || rnnMode == miopenRNNTANH)
                    {
                        // activation
                        activDesc.Backward(handle,
                                           &alpha,
                                           sp_desc,
                                           reserveSpace,
                                           sp_desc,
                                           workSpace,
                                           sp_desc,
                                           reserveSpace,
                                           &beta,
                                           sp_desc,
                                           workSpace,
                                           offset + static_cast<size_t>(ri) * wei_len +
                                               static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                           offset + static_cast<size_t>(ri) * wei_len,
                                           offset + static_cast<size_t>(ri) * wei_len,
                                           offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenLSTM)
                    {
                        if(algoMode == miopenRNNdefault)
                        {
                            LSTMBackwardHiddenStateUpdate(
                                handle,
                                wDesc.GetType(),
                                ti == 0,
                                ti == seqLen - 1,
                                ri,
                                in_n.at(0),
                                in_n.at(cur_time),
                                in_n.at(use_time),
                                in_n.at(use_time2),
                                hy_h,
                                hy_stride,
                                wei_len,
                                wei_stride,
                                cx,
                                hx_shift + ri * hy_n * hy_h,
                                reserveSpace,
                                offset + static_cast<size_t>(ri) * wei_len,
                                offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                offset + 2 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + 3 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                (li * batch_n + cur_batch) * bi * hy_h + ri * hy_h +
                                    nLayers * batch_n * hy_stride,
                                li * batch_n * hy_stride + pre_batch2 * hy_stride + bi * wei_len +
                                    ri * hy_h,
                                dcy,
                                hx_shift + ri * hy_n * hy_h,
                                workSpace,
                                offset + static_cast<size_t>(ri) * wei_len,
                                offset + hy_h + static_cast<size_t>(ri) * wei_len,
                                offset + 2 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + 3 * static_cast<size_t>(hy_h) +
                                    static_cast<size_t>(ri) * wei_len,
                                offset + static_cast<size_t>(bi) * wei_len +
                                    static_cast<size_t>(ri) * hy_h,
                                li * batch_n * hy_stride + pre_batch * hy_stride + bi * wei_len +
                                    ri * hy_h,
                                offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                li * batch_n * hy_stride + pre_batch * hy_stride + hy_h +
                                    ri * wei_len);

                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                            continue;
                        }

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        // update cell state
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        tanhDesc.Backward(handle,
                                          &alpha,
                                          sp_desc,
                                          reserveSpace,
                                          sp_desc,
                                          workSpace,
                                          sp_desc,
                                          reserveSpace,
                                          &beta,
                                          sp_desc,
                                          workSpace,
                                          offset + static_cast<size_t>(bi) * wei_len +
                                              static_cast<size_t>(ri) * hy_h +
                                              nLayers * batch_n * hy_stride,
                                          offset + static_cast<size_t>(bi) * wei_len +
                                              static_cast<size_t>(ri) * hy_h,
                                          offset + static_cast<size_t>(bi) * wei_len +
                                              static_cast<size_t>(ri) * hy_h,
                                          offset + static_cast<size_t>(bi) * wei_len +
                                              static_cast<size_t>(ri) * hy_h);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        if(ti == seqLen - 1)
                        {
                            if(dcy != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpAdd,
                                         &alpha0,
                                         hx_desc,
                                         dcy,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 0 && dcy != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                hx_size[2] = hy_h;
                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time);
                                sp_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpAdd,
                                         &alpha0,
                                         hx_desc,
                                         dcy,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            pretime_shift = li * batch_n * hy_stride + pre_batch * hy_stride;
                            alpha0        = 1;
                            alpha1        = 1;
                            beta_t        = 1;

                            if(in_n.at(cur_time) != in_n.at(use_time))
                            {
                                sp_size[1] = in_n.at(use_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            OpTensor(handle,
                                     miopenTensorOpMul,
                                     &alpha0,
                                     sp_desc,
                                     workSpace,
                                     &alpha1,
                                     sp_desc,
                                     reserveSpace,
                                     &beta_t,
                                     sp_desc,
                                     workSpace,
                                     pretime_shift + static_cast<size_t>(bi) * wei_len +
                                         static_cast<size_t>(ri) * hy_h,
                                     pretime_shift + hy_h + ri * wei_len +
                                         nLayers * batch_n * hy_stride,
                                     offset + static_cast<size_t>(bi) * wei_len +
                                         static_cast<size_t>(ri) * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);

                            if(in_n.at(cur_time) != in_n.at(use_time))
                            {
                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }
                        }

                        // update forget gate
                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        if(ti == 0)
                        {
                            if(cx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len);
                            }
                        }
                        else
                        {
                            if(ri == 1 && cx != nullptr && in_n.at(cur_time) > in_n.at(use_time2))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time2);
                                hx_size[2] = hy_h;
                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time2);
                                sp_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         hx_desc,
                                         cx,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time2)) * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time2) * hy_h,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time2)) * hy_stride);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time2) > 0)
                            {
                                pretime_shift = li * batch_n * hy_stride + pre_batch2 * hy_stride;

                                if(in_n.at(cur_time) != in_n.at(use_time2))
                                {
                                    sp_size[1] = in_n.at(use_time2);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         pretime_shift + static_cast<size_t>(bi) * wei_len +
                                             static_cast<size_t>(ri) * hy_h,
                                         offset + hy_h + static_cast<size_t>(ri) * wei_len);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                if(in_n.at(cur_time) != in_n.at(use_time2))
                                {
                                    sp_size[1] = in_n.at(cur_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }
                            }
                        }

                        // update input gate
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h,
                                 offset + 3 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     nLayers * batch_n * hy_stride,
                                 offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update output gate
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h + nLayers * batch_n * hy_stride,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // update c gate
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + static_cast<size_t>(bi) * wei_len +
                                     static_cast<size_t>(ri) * hy_h,
                                 offset + static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + 3 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        tanhDesc.Backward(handle,
                                          &alpha,
                                          sp_desc,
                                          reserveSpace,
                                          sp_desc,
                                          workSpace,
                                          sp_desc,
                                          reserveSpace,
                                          &beta,
                                          sp_desc,
                                          workSpace,
                                          offset + 3 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len +
                                              nLayers * batch_n * hy_stride,
                                          offset + 3 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len,
                                          offset + 3 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len,
                                          offset + 3 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        sp_size[2] = 3 * hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                        sigDesc.Backward(handle,
                                         &alpha,
                                         sp_desc,
                                         reserveSpace,
                                         sp_desc,
                                         workSpace,
                                         sp_desc,
                                         reserveSpace,
                                         &beta,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                    else if(rnnMode == miopenGRU)
                    {
                        // c gate
                        alpha0 = 1;
                        alpha1 = -1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                 offset + static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        alpha0 = 1;
                        alpha1 = 1;
                        beta_t = 0;

                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        tanhDesc.Backward(handle,
                                          &alpha,
                                          sp_desc,
                                          reserveSpace,
                                          sp_desc,
                                          workSpace,
                                          sp_desc,
                                          reserveSpace,
                                          &beta,
                                          sp_desc,
                                          workSpace,
                                          offset + 2 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len +
                                              static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                          offset + 2 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len,
                                          offset + 2 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len,
                                          offset + 2 * static_cast<size_t>(hy_h) +
                                              static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // r gate
                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h +
                                     nLayers * batch_n * hy_stride,
                                 offset + hy_h + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 sp_desc,
                                 reserveSpace,
                                 &beta_t,
                                 sp_desc,
                                 reserveSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len,
                                 offset + hy_h + static_cast<size_t>(ri) * wei_len +
                                     nLayers * batch_n * hy_stride,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h +
                                     nLayers * batch_n * hy_stride);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        // z gate
                        if(ti == 0)
                        {
                            if(hx != nullptr)
                            {
                                hx_size[1] = in_n.at(cur_time);
                                hx_size[2] = hy_h;
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         hx_desc,
                                         hx,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         hx_shift + ri * hy_n * hy_h,
                                         offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(ri) * wei_len);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time2))
                            {
                                hx_size[1] = in_n.at(cur_time) - in_n.at(use_time2);
                                hx_size[2] = hy_h;
                                sp_size[1] = in_n.at(cur_time) - in_n.at(use_time2);
                                hx_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         hx_desc,
                                         hx,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         hx_shift + ri * hy_n * hy_h + in_n.at(use_time2) * hy_h,
                                         offset + dhd_off + static_cast<size_t>(ri) * hy_h +
                                             static_cast<size_t>(in_n.at(use_time2)) * hy_stride,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(in_n.at(use_time2)) * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                sp_size[1] = in_n.at(cur_time);
                                sp_desc =
                                    miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                            }

                            if(in_n.at(use_time2) > 0)
                            {
                                if(in_n.at(use_time2) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(use_time2);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         reserveSpace,
                                         &alpha1,
                                         sp_desc,
                                         workSpace,
                                         &beta_t,
                                         sp_desc,
                                         workSpace,
                                         hid_shift + pre_batch2 * hy_stride + dhd_off + ri * hy_h,
                                         offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                         offset + static_cast<size_t>(ri) * wei_len);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                if(in_n.at(use_time2) != in_n.at(cur_time))
                                {
                                    sp_size[1] = in_n.at(cur_time);
                                    sp_desc    = miopen::TensorDescriptor(
                                        wDesc.GetType(), sp_size, sp_stride);
                                }
                            }
                        }

                        alpha0 = -1;
                        alpha1 = 1;
                        beta_t = 1;

                        OpTensor(handle,
                                 miopenTensorOpMul,
                                 &alpha0,
                                 sp_desc,
                                 reserveSpace,
                                 &alpha1,
                                 sp_desc,
                                 workSpace,
                                 &beta_t,
                                 sp_desc,
                                 workSpace,
                                 offset + 2 * static_cast<size_t>(hy_h) +
                                     static_cast<size_t>(ri) * wei_len +
                                     static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                 offset + dhd_off + static_cast<size_t>(ri) * hy_h,
                                 offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);

                        sp_size[2] = 2 * hy_h;
                        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                        sigDesc.Backward(handle,
                                         &alpha,
                                         sp_desc,
                                         reserveSpace,
                                         sp_desc,
                                         workSpace,
                                         sp_desc,
                                         reserveSpace,
                                         &beta,
                                         sp_desc,
                                         workSpace,
                                         offset + static_cast<size_t>(ri) * wei_len +
                                             static_cast<size_t>(nLayers) * batch_n * hy_stride,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         offset + static_cast<size_t>(ri) * wei_len,
                                         offset + static_cast<size_t>(ri) * wei_len);
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                }
            }

            baccbi += in_n.at(seqLen - 1 - ti);
        }

        // dcx, dhx
        if(dhx != nullptr || (rnnMode == miopenLSTM && dcx != nullptr))
        {
            hx_size[2] = hy_h;
            sp_size[2] = hy_h;

            bacc   = 0;
            baccbi = batch_n;
            for(int ti = 0; ti < seqLen; ti++)
            {
                baccbi -= in_n.at(seqLen - 1 - ti);
                for(int ri = 0; ri < bi; ri++)
                {
                    cur_time      = ri == 0 ? ti : seqLen - 1 - ti;
                    cur_batch     = ri == 0 ? bacc : baccbi;
                    use_time      = 0;
                    int use_batch = 0;

                    if(ti > 0)
                    {
                        use_time  = ri == 0 ? ti - 1 : seqLen - ti;
                        use_batch = in_n.at(use_time);
                    }

                    if(in_n.at(cur_time) > use_batch)
                    {
                        pretime_shift = li * batch_n * hy_stride + cur_batch * hy_stride;

                        if(rnnMode == miopenLSTM || rnnMode == miopenGRU)
                        {
                            sp_size[1] = in_n.at(cur_time) - use_batch;
                            hx_size[1] = in_n.at(cur_time) - use_batch;
                            hx_desc = miopen::TensorDescriptor(wDesc.GetType(), hx_size, hx_stride);
                            sp_desc = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);
                        }

                        if(dhx != nullptr)
                        {
                            if(rnnMode == miopenGRU)
                            {
                                alpha0 = 1;
                                alpha1 = 1;
                                beta_t = 0;

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         sp_desc,
                                         reserveSpace,
                                         pretime_shift + 2 * hy_h + ri * wei_len +
                                             use_batch * hy_stride,
                                         pretime_shift + hy_h + ri * wei_len +
                                             use_batch * hy_stride + nLayers * batch_n * hy_stride,
                                         pretime_shift + dhd_off + ri * hy_h +
                                             use_batch * hy_stride + nLayers * batch_n * hy_stride);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                                miopen::GemmDescriptor gemm_desc =
                                    GemmDescriptor{false,
                                                   false,
                                                   false,
                                                   (in_n.at(cur_time) - use_batch),
                                                   hy_h,
                                                   hy_h,
                                                   hy_stride,
                                                   uni_stride,
                                                   uni_stride,
                                                   1, // batch count
                                                   0, // Stride A
                                                   0, // Stride B
                                                   0, // Stride C
                                                   1, // alpha
                                                   0, // beta
                                                   yDesc[0].GetType(),
                                                   false};

                                miopenStatus_t gemm_status = CallGemm(
                                    handle,
                                    gemm_desc,
                                    reserveSpace,
                                    pretime_shift + dhd_off + ri * hy_h + use_batch * hy_stride +
                                        static_cast<int>(nLayers) * batch_n * hy_stride,
                                    w,
                                    weitime_shift + 2 * hy_h * uni_stride +
                                        ri * wei_len * uni_stride,
                                    dhx,
                                    hx_shift + ri * hy_n * hy_h + use_batch * hy_h,
                                    GemmBackend_t::miopengemm);

                                if(gemm_status != miopenStatusSuccess)
                                {
                                    if(gemm_status == miopenStatusNotImplemented)
                                    {
                                        MIOPEN_LOG_E("GEMM not implemented");
                                    }
                                    else
                                    {
                                        MIOPEN_LOG_E("GEMM failed");
                                    }
                                }
                                // Update time
                                profileRNNkernels(handle, 1, ctime);

                                beta_t = 1;

                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         hx_desc,
                                         dhx,
                                         pretime_shift + dhd_off + ri * hy_h +
                                             use_batch * hy_stride,
                                         pretime_shift + ri * wei_len + use_batch * hy_stride +
                                             nLayers * batch_n * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }

                            miopen::GemmDescriptor gemm_desc =
                                GemmDescriptor{false,
                                               false,
                                               false,
                                               (in_n.at(cur_time) - use_batch),
                                               hy_h,
                                               wei_len_t,
                                               hy_stride,
                                               uni_stride,
                                               uni_stride,
                                               1, // batch count
                                               0, // Stride A
                                               0, // Stride B
                                               0, // Stride C
                                               1, // alpha
                                               1, // beta
                                               yDesc[0].GetType(),
                                               false};

                            miopenStatus_t gemm_status =
                                CallGemm(handle,
                                         gemm_desc,
                                         workSpace,
                                         pretime_shift + ri * wei_len + use_batch * hy_stride,
                                         w,
                                         weitime_shift + ri * wei_len * uni_stride,
                                         dhx,
                                         hx_shift + ri * hy_n * hy_h + use_batch * hy_h,
                                         GemmBackend_t::miopengemm);

                            if(gemm_status != miopenStatusSuccess)
                            {
                                if(gemm_status == miopenStatusNotImplemented)
                                {
                                    MIOPEN_LOG_E("GEMM not implemented");
                                }
                                else
                                {
                                    MIOPEN_LOG_E("GEMM failed");
                                }
                            }
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }

                        if(rnnMode == miopenLSTM && dcx != nullptr)
                        {
                            alpha0 = 1;
                            alpha1 = 1;
                            beta_t = 1;
                            if(algoMode == miopenRNNdefault)
                            {
                                OpTensor(handle,
                                         miopenTensorOpMul,
                                         &alpha0,
                                         sp_desc,
                                         workSpace,
                                         &alpha1,
                                         sp_desc,
                                         reserveSpace,
                                         &beta_t,
                                         hx_desc,
                                         dcx,
                                         pretime_shift + bi * wei_len + ri * hy_h +
                                             static_cast<size_t>(use_batch) * hy_stride,
                                         pretime_shift + hy_h + ri * wei_len +
                                             use_batch * hy_stride,
                                         hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                                continue;
                            }
                            OpTensor(handle,
                                     miopenTensorOpMul,
                                     &alpha0,
                                     sp_desc,
                                     workSpace,
                                     &alpha1,
                                     sp_desc,
                                     reserveSpace,
                                     &beta_t,
                                     hx_desc,
                                     dcx,
                                     pretime_shift + bi * wei_len + ri * hy_h +
                                         static_cast<size_t>(use_batch) * hy_stride,
                                     pretime_shift + hy_h + ri * wei_len + use_batch * hy_stride +
                                         nLayers * batch_n * hy_stride,
                                     hx_shift + ri * hy_n * hy_h + use_batch * hy_h);
                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                    }
                }
                bacc += in_n.at(ti);
            }
        }
    }

    // dinput
    if(inputMode == miopenRNNskip)
    {
        sp_size[1] = batch_n;
        sp_size[2] = hy_h;
        x_size[1]  = batch_n;
        x_size[2]  = hy_h;
        x_desc     = miopen::TensorDescriptor(wDesc.GetType(), x_size, x_stride);
        sp_desc    = miopen::TensorDescriptor(wDesc.GetType(), sp_size, sp_stride);

        alpha0 = 1;
        alpha1 = 1;
        beta_t = 0;

        for(int gi = 0; gi < nHiddenTensorsPerLayer * bi; gi++)
        {
            OpTensor(handle,
                     miopenTensorOpAdd,
                     &alpha0,
                     sp_desc,
                     workSpace,
                     &alpha1,
                     x_desc,
                     dx,
                     &beta_t,
                     x_desc,
                     dx,
                     static_cast<size_t>(gi) * hy_h,
                     0,
                     0);
            // Update time
            profileRNNkernels(handle, (gi == nHiddenTensorsPerLayer * bi - 1) ? 2 : 1, ctime);
        }
    }
    else
    {
        miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                          false,
                                                          false,
                                                          batch_n,
                                                          in_h,
                                                          wei_len * bi,
                                                          hy_stride,
                                                          in_stride,
                                                          in_stride,
                                                          1, // batch count
                                                          0, // Stride A
                                                          0, // Stride B
                                                          0, // Stride C
                                                          1, // alpha
                                                          0, // beta
                                                          yDesc[0].GetType(),
                                                          false};
        miopenStatus_t gemm_status =
            CallGemm(handle, gemm_desc, workSpace, 0, w, 0, dx, 0, GemmBackend_t::miopengemm);
        if(gemm_status != miopenStatusSuccess)
        {
            if(gemm_status == miopenStatusNotImplemented)
            {
                MIOPEN_LOG_E("GEMM not implemented");
            }
            else
            {
                MIOPEN_LOG_E("GEMM failed");
            }
        }
        // Update time
        profileRNNkernels(handle, 2, ctime);
    }

#else

    (void)handle;
    (void)seqLen;
    (void)dhy;
    (void)dcy;
    (void)dyDesc;
    (void)dy;
    (void)w;
    (void)hx;
    (void)cx;
    (void)dxDesc;
    (void)dx;
    (void)workSpace;
    (void)workSpaceSize;
    (void)reserveSpace;
    (void)reserveSpaceSize;
    MIOPEN_THROW("GEMM is not supported");
#endif
};

void RNNDescriptor::RNNBackwardWeights(Handle& handle,
                                       const int seqLen,
                                       c_array_view<const miopenTensorDescriptor_t> xDesc,
                                       ConstData_t x,
                                       const TensorDescriptor& hxDesc,
                                       ConstData_t hx,
                                       c_array_view<const miopenTensorDescriptor_t> dyDesc,
                                       ConstData_t dy,
                                       const TensorDescriptor& dwDesc,
                                       Data_t dw,
                                       Data_t workSpace,
                                       size_t workSpaceSize,
                                       ConstData_t reserveSpace,
                                       size_t reserveSpaceSize) const
{

#if MIOPEN_USE_GEMM
    float ctime = 0.;
    // reset kernel timer
    profileRNNkernels(handle, 0, ctime);

    if(x == nullptr || dw == nullptr || dy == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }
    if(workSpaceSize < GetWorkspaceSize(handle, seqLen, xDesc))
    {
        MIOPEN_THROW("Workspace is required");
    }
    if(reserveSpaceSize < GetReserveSize(handle, seqLen, xDesc))
    {
        MIOPEN_THROW("Reservespace is required");
    }

    std::string network_config;
    std::vector<int> in_n;
    int in_h  = xDesc[0].GetLengths()[1];
    int hy_d  = hxDesc.GetLengths()[0];
    int hy_n  = hxDesc.GetLengths()[1];
    int hy_h  = hxDesc.GetLengths()[2];
    int out_h = dyDesc[0].GetLengths()[1];

    if(in_h <= 0 || hy_h <= 0 || hy_n <= 0 || hy_d <= 0 || out_h <= 0 || seqLen <= 0)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    int batch_n = 0;
    for(int i = 0; i < seqLen; i++)
    {
        int batchval, inputvec, batchvalout, outputvec;
        std::tie(batchval, inputvec)     = miopen::tien<2>(xDesc[i].GetLengths());
        std::tie(batchvalout, outputvec) = miopen::tien<2>(dyDesc[i].GetLengths());
        if(batchval != batchvalout)
        {
            MIOPEN_THROW(miopenStatusBadParm);
        }
        if(i == 0)
        {
            if(batchval <= 0)
            {
                MIOPEN_THROW(miopenStatusBadParm, "Input batch is ZERO!");
            }
        }
        else
        {
            if(batchval > in_n.back() || batchval < 0)
            {
                MIOPEN_THROW(miopenStatusBadParm,
                             "Incorrect input batch size at time " + std::to_string(i) +
                                 "! Batch size must not ascend!");
            }
        }
        in_n.push_back(batchval);
        batch_n += xDesc[i].GetLengths()[0];
    }

    int bi = dirMode != 0u ? 2 : 1;
    if(out_h != (bi * hy_h))
    {
        MIOPEN_THROW(miopenStatusBadParm, "Output size doesn't match hidden state size!");
    }

    int in_stride  = in_h;
    int hy_stride  = hy_h * bi * static_cast<int>(workspaceScale);
    int wei_stride = hy_h * bi * static_cast<int>(nHiddenTensorsPerLayer);
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    if(inputMode == miopenRNNskip)
    {
        if(in_h != hy_h)
        {
            MIOPEN_THROW(miopenStatusBadParm,
                         "The input tensor size must equal to the hidden "
                         "state size of the network in SKIP_INPUT mode!");
        }
        in_h = 0;
    }

    size_t wei_shift_bias = (in_h + hy_h + (bi * hy_h + hy_h) * (nLayers - 1)) * wei_stride;

    float alpha0, alpha1, beta_t = 0;

    std::vector<int> sp_size(3, 1), sp_stride(3, 1), w_size(3, 1), w_stride(3, 1);
    miopen::TensorDescriptor sp_desc, w_desc;

    sp_stride[0] = batch_n * hy_stride;
    sp_stride[1] = hy_stride;
    w_size[2]    = dwDesc.GetElementSize();
    w_stride[0]  = w_size[2];
    w_stride[1]  = w_size[2];
    w_desc       = miopen::TensorDescriptor(dwDesc.GetType(), w_size, w_stride);
    SetTensor(handle, w_desc, dw, &beta_t);
    // Update time
    profileRNNkernels(handle, 1, ctime);
    w_stride[0] = wei_stride;
    w_stride[1] = wei_stride;
    w_size[2]   = 1;

    int wei_len   = 0;
    int hid_off   = 0;
    int use_time  = 0;
    int pre_batch = 0;

    switch(rnnMode)
    {
    case miopenRNNRELU:
    case miopenRNNTANH:
        // printf("run rnn gpu bwd weights \n");
        wei_len = hy_h;
        hid_off = static_cast<int>(nLayers) * batch_n * hy_stride;
        break;
    case miopenLSTM:
        // printf("run lstm gpu bwd weights \n");
        wei_len = hy_h * 4;
        hid_off = bi * hy_h * 5;
        break;
    case miopenGRU:
        // printf("run gru gpu bwd weights \n");
        wei_len = hy_h * 3;
        hid_off = bi * hy_h * 3;
        break;
    }

    for(int li = 0; li < nLayers; li++)
    {
        int hid_shift = li * batch_n * hy_stride;
        int wei_shift = (in_h + hy_h) * wei_stride + (li - 1) * (bi * hy_h + hy_h) * wei_stride;

        // between layers
        if(li == 0)
        {
            if(inputMode == miopenRNNlinear)
            {
                miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                  true,
                                                                  false,
                                                                  wei_len * bi,
                                                                  in_h,
                                                                  batch_n,
                                                                  hy_stride,
                                                                  in_stride,
                                                                  in_stride,
                                                                  1, // batch count
                                                                  0, // Stride A
                                                                  0, // Stride B
                                                                  0, // Stride C
                                                                  1, // alpha
                                                                  1, // beta
                                                                  xDesc[0].GetType(),
                                                                  false};

                miopenStatus_t gemm_status = CallGemm(
                    handle, gemm_desc, workSpace, 0, x, 0, dw, 0, GemmBackend_t::miopengemm);

                if(gemm_status != miopenStatusSuccess)
                {
                    if(gemm_status == miopenStatusNotImplemented)
                    {
                        MIOPEN_LOG_E("GEMM not implemented");
                    }
                    else
                    {
                        MIOPEN_LOG_E("GEMM failed");
                    }
                }
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }
        else
        {
            bool use_dropout    = !float_equal(miopen::deref(dropoutDesc).dropout, 0);
            auto prelayer_shift = static_cast<int>(
                use_dropout ? (algoMode == miopenRNNdefault && rnnMode == miopenLSTM
                                   ? nLayers * batch_n * hy_stride + nLayers * batch_n * hy_h * bi
                                   : 2 * nLayers * batch_n * hy_stride) +
                                  (static_cast<size_t>(li) - 1) * batch_n * hy_h * bi
                            : (li - 1) * batch_n * hy_stride + hid_off);

            miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                              true,
                                                              false,
                                                              wei_len * bi,
                                                              hy_h * bi,
                                                              batch_n,
                                                              hy_stride,
                                                              use_dropout ? hy_h * bi : hy_stride,
                                                              bi_stride,
                                                              1, // batch count
                                                              0, // Stride A
                                                              0, // Stride B
                                                              0, // Stride C
                                                              1, // alpha
                                                              1, // beta
                                                              xDesc[0].GetType(),
                                                              false};

            miopenStatus_t gemm_status = CallGemm(handle,
                                                  gemm_desc,
                                                  workSpace,
                                                  hid_shift,
                                                  reserveSpace,
                                                  prelayer_shift,
                                                  dw,
                                                  wei_shift,
                                                  GemmBackend_t::miopengemm);

            if(gemm_status != miopenStatusSuccess)
            {
                if(gemm_status == miopenStatusNotImplemented)
                {
                    MIOPEN_LOG_E("GEMM not implemented");
                }
                else
                {
                    MIOPEN_LOG_E("GEMM failed");
                }
            }
            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        if(biasMode != 0u)
        {
            wei_shift = static_cast<int>(wei_shift_bias) + li * 2 * wei_stride;

            sp_size[1] = batch_n;
            sp_size[2] = wei_stride;
            w_size[1]  = 1;
            w_size[2]  = wei_stride;
            w_desc     = miopen::TensorDescriptor(dwDesc.GetType(), w_size, w_stride);
            sp_desc    = miopen::TensorDescriptor(dwDesc.GetType(), sp_size, sp_stride);

            alpha0 = 0;
            alpha1 = 1;
            beta_t = 1;

            OpTensor(handle,
                     miopenTensorOpAdd,
                     &alpha0,
                     w_desc,
                     dw,
                     &alpha1,
                     sp_desc,
                     workSpace,
                     &beta_t,
                     w_desc,
                     dw,
                     wei_shift,
                     hid_shift,
                     wei_shift);

            // Update time
            profileRNNkernels(handle, 1, ctime);
        }

        // between time
        // Calculate feedback for c gate in GRU
        if(rnnMode == miopenGRU)
        {
            sp_size[1] = batch_n;
            sp_size[2] = hy_h;
            sp_desc    = miopen::TensorDescriptor(dwDesc.GetType(), sp_size, sp_stride);

            for(int ri = 0; ri < bi; ri++)
            {
                CopyTensor(handle,
                           sp_desc,
                           reserveSpace,
                           sp_desc,
                           workSpace,
                           hid_shift + hid_off + ri * hy_h +
                               static_cast<int>(nLayers) * batch_n * hy_stride,
                           hid_shift + 2 * hy_h + ri * wei_len);
                // Update time
                profileRNNkernels(handle, 1, ctime);
            }
        }

        if(biasMode != 0u)
        {
            wei_shift = static_cast<int>(wei_shift_bias) + li * 2 * wei_stride + wei_stride;

            alpha0 = 1;
            alpha1 = 1;
            beta_t = 0;

            if(hx != nullptr)
            {
                if(rnnMode == miopenGRU)
                {
                    sp_size[1] = batch_n;
                    sp_size[2] = wei_stride;
                    w_size[1]  = 1;
                    w_size[2]  = wei_stride;
                    w_desc     = miopen::TensorDescriptor(dwDesc.GetType(), w_size, w_stride);
                    sp_desc    = miopen::TensorDescriptor(dwDesc.GetType(), sp_size, sp_stride);

                    OpTensor(handle,
                             miopenTensorOpAdd,
                             &alpha0,
                             w_desc,
                             dw,
                             &alpha1,
                             sp_desc,
                             workSpace,
                             &beta_t,
                             w_desc,
                             dw,
                             wei_shift,
                             hid_shift,
                             wei_shift);

                    // Update time
                    profileRNNkernels(handle, 1, ctime);
                }
                else
                {
                    CopyTensor(handle, w_desc, dw, w_desc, dw, wei_shift - wei_stride, wei_shift);
                    // Update time
                    profileRNNkernels(handle, 1, ctime);
                }
            }
            else
            {
                sp_size[1] = 1;
                sp_size[2] = wei_len;
                w_size[1]  = 1;
                w_size[2]  = wei_len;
                w_desc     = miopen::TensorDescriptor(dwDesc.GetType(), w_size, w_stride);
                sp_desc    = miopen::TensorDescriptor(dwDesc.GetType(), sp_size, sp_stride);

                for(int bs = 0; bs < batch_n; bs++)
                {
                    if(!(hx == nullptr && bs < in_n.at(0)))
                    {
                        OpTensor(handle,
                                 miopenTensorOpAdd,
                                 &alpha0,
                                 sp_desc,
                                 workSpace,
                                 &alpha1,
                                 w_desc,
                                 dw,
                                 &beta_t,
                                 w_desc,
                                 dw,
                                 hid_shift + bs * hy_stride,
                                 wei_shift,
                                 wei_shift);

                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }
                }

                if(dirMode != 0u)
                {
                    sp_size[1] = 1;
                    sp_size[2] = wei_len;
                    w_size[1]  = 1;
                    w_size[2]  = wei_len;
                    w_desc     = miopen::TensorDescriptor(dwDesc.GetType(), w_size, w_stride);
                    sp_desc    = miopen::TensorDescriptor(dwDesc.GetType(), sp_size, sp_stride);

                    int cur_batch = 0;
                    for(int ti = 0; ti < seqLen - 1; ti++)
                    {
                        for(int bs = 0; bs < in_n.at(ti + 1); bs++)
                        {
                            OpTensor(handle,
                                     miopenTensorOpAdd,
                                     &alpha0,
                                     sp_desc,
                                     workSpace,
                                     &alpha1,
                                     w_desc,
                                     dw,
                                     &beta_t,
                                     w_desc,
                                     dw,
                                     hid_shift + (cur_batch + bs) * hy_stride + wei_len,
                                     wei_shift + wei_len,
                                     wei_shift + wei_len);

                            // Update time
                            profileRNNkernels(handle, 1, ctime);
                        }
                        cur_batch += in_n.at(ti);
                    }
                }
            }
        }

        int pretime_shift, hx_shift, cur_time;
        bool comb_check = true;
        if(seqLen > 2)
        {
            if(in_n.at(0) != in_n.at(seqLen - 2))
            {
                comb_check = false;
            }
        }

        if(comb_check)
        {
            hx_shift  = li * hy_n * bi_stride;
            wei_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

            for(int ri = 0; ri < bi; ri++)
            {
                hid_shift =
                    ri == 0 ? li * batch_n * hy_stride
                            : (li * batch_n * hy_stride + in_n.at(0) * (seqLen - 1) * hy_stride);
                cur_time = ri == 0 ? 0 : seqLen - 1;

                if(in_n.at(cur_time) > 0 && hx != nullptr)
                {
                    miopen::GemmDescriptor gemm_desc = GemmDescriptor{false,
                                                                      true,
                                                                      false,
                                                                      wei_len,
                                                                      hy_h,
                                                                      in_n.at(cur_time),
                                                                      hy_stride,
                                                                      uni_stride,
                                                                      uni_stride,
                                                                      1, // batch count
                                                                      0, // Stride A
                                                                      0, // Stride B
                                                                      0, // Stride C
                                                                      1, // alpha
                                                                      1, // beta
                                                                      xDesc[0].GetType(),
                                                                      false};

                    miopenStatus_t gemm_status = CallGemm(handle,
                                                          gemm_desc,
                                                          workSpace,
                                                          hid_shift + ri * wei_len,
                                                          hx,
                                                          hx_shift + ri * hy_n * hy_h,
                                                          dw,
                                                          wei_shift + ri * wei_len * uni_stride,
                                                          GemmBackend_t::miopengemm);

                    if(gemm_status != miopenStatusSuccess)
                    {
                        if(gemm_status == miopenStatusNotImplemented)
                        {
                            MIOPEN_LOG_E("GEMM not implemented");
                        }
                        else
                        {
                            MIOPEN_LOG_E("GEMM failed");
                        }
                    }

                    // Update time
                    if(li == nLayers - 1 && ri == bi - 1 && seqLen == 1)
                        profileRNNkernels(handle, 2, ctime);
                    else
                        profileRNNkernels(handle, 1, ctime);
                }

                if(seqLen > 1)
                {
                    if(ri == 1 && hx != nullptr && in_n.at(0) > in_n.at(seqLen - 1))
                    {
                        miopen::GemmDescriptor gemm_desc =
                            GemmDescriptor{false,
                                           true,
                                           false,
                                           wei_len,
                                           hy_h,
                                           (in_n.at(0) - in_n.at(seqLen - 1)),
                                           hy_stride,
                                           uni_stride,
                                           uni_stride,
                                           1, // batch count
                                           0, // Stride A
                                           0, // Stride B
                                           0, // Stride C
                                           1, // alpha
                                           1, // beta
                                           xDesc[0].GetType(),
                                           false};

                        miopenStatus_t gemm_status =
                            CallGemm(handle,
                                     gemm_desc,
                                     workSpace,
                                     hid_shift + ri * wei_len -
                                         (in_n.at(0) - in_n.at(seqLen - 1)) * hy_stride,
                                     hx,
                                     hx_shift + ri * hy_n * hy_h + in_n.at(seqLen - 1) * hy_h,
                                     dw,
                                     wei_shift + ri * wei_len * uni_stride,
                                     GemmBackend_t::miopengemm);

                        if(gemm_status != miopenStatusSuccess)
                        {
                            if(gemm_status == miopenStatusNotImplemented)
                            {
                                MIOPEN_LOG_E("GEMM not implemented");
                            }
                            else
                            {
                                MIOPEN_LOG_E("GEMM failed");
                            }
                        }
                        // Update time
                        profileRNNkernels(handle, 1, ctime);
                    }

                    hid_shift = ri == 0 ? (li * batch_n * hy_stride + in_n.at(0) * hy_stride)
                                        : (li * batch_n * hy_stride);
                    pretime_shift =
                        ri == 0 ? li * batch_n * hy_stride + hid_off
                                : li * batch_n * hy_stride + in_n.at(0) * hy_stride + hid_off;

                    miopen::GemmDescriptor gemm_desc =
                        GemmDescriptor{false,
                                       true,
                                       false,
                                       wei_len,
                                       hy_h,
                                       in_n.at(0) * (seqLen - 2) + in_n.at(seqLen - 1),
                                       hy_stride,
                                       hy_stride,
                                       uni_stride,
                                       1, // batch count
                                       0, // Stride A
                                       0, // Stride B
                                       0, // Stride C
                                       1, // alpha
                                       1, // beta
                                       xDesc[0].GetType(),
                                       false};

                    miopenStatus_t gemm_status = CallGemm(handle,
                                                          gemm_desc,
                                                          workSpace,
                                                          hid_shift + ri * wei_len,
                                                          reserveSpace,
                                                          pretime_shift + ri * hy_h,
                                                          dw,
                                                          wei_shift + ri * wei_len * uni_stride,
                                                          GemmBackend_t::miopengemm);

                    if(gemm_status != miopenStatusSuccess)
                    {
                        if(gemm_status == miopenStatusNotImplemented)
                        {
                            MIOPEN_LOG_E("GEMM not implemented");
                        }
                        else
                        {
                            MIOPEN_LOG_E("GEMM failed");
                        }
                    }
                    // Update time
                    if(li == nLayers - 1 && ri == bi - 1)
                        profileRNNkernels(handle, 2, ctime);
                    else
                        profileRNNkernels(handle, 1, ctime);
                }
            }
        }
        else
        {
            int bacc   = 0;
            int baccbi = batch_n;
            for(int ti = 0; ti < seqLen; ti++)
            {
                baccbi -= in_n.at(seqLen - 1 - ti);

                hx_shift  = li * hy_n * bi_stride;
                wei_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

                for(int ri = 0; ri < bi; ri++)
                {
                    hid_shift = ri == 0 ? (li * batch_n * hy_stride + bacc * hy_stride)
                                        : (li * batch_n * hy_stride + baccbi * hy_stride);
                    cur_time  = ri == 0 ? ti : seqLen - 1 - ti;
                    if(ti > 0)
                    {
                        pre_batch =
                            ri == 0 ? bacc - in_n.at(ti - 1) : baccbi + in_n.at(seqLen - 1 - ti);
                        use_time = ri == 0 ? ti : seqLen - ti;
                    }

                    if(in_n.at(cur_time) > 0)
                    {
                        if(ti == 0)
                        {
                            if(hx != nullptr)
                            {
                                miopen::GemmDescriptor gemm_desc =
                                    GemmDescriptor{false,
                                                   true,
                                                   false,
                                                   wei_len,
                                                   hy_h,
                                                   in_n.at(cur_time),
                                                   hy_stride,
                                                   uni_stride,
                                                   uni_stride,
                                                   1, // batch count
                                                   0, // Stride A
                                                   0, // Stride B
                                                   0, // Stride C
                                                   1, // alpha
                                                   1, // beta
                                                   xDesc[0].GetType(),
                                                   false};

                                miopenStatus_t gemm_status =
                                    CallGemm(handle,
                                             gemm_desc,
                                             workSpace,
                                             hid_shift + ri * wei_len,
                                             hx,
                                             hx_shift + ri * hy_n * hy_h,
                                             dw,
                                             wei_shift + ri * wei_len * uni_stride,
                                             GemmBackend_t::miopengemm);

                                if(gemm_status != miopenStatusSuccess)
                                {
                                    if(gemm_status == miopenStatusNotImplemented)
                                    {
                                        MIOPEN_LOG_E("GEMM not implemented");
                                    }
                                    else
                                    {
                                        MIOPEN_LOG_E("GEMM failed");
                                    }
                                }
                                // Update time
                                if(li == nLayers - 1 && ti == seqLen - 1 && ri == bi - 1)
                                    profileRNNkernels(handle, 2, ctime);
                                else
                                    profileRNNkernels(handle, 1, ctime);
                            }
                        }
                        else
                        {
                            if(ri == 1 && hx != nullptr && in_n.at(cur_time) > in_n.at(use_time))
                            {
                                miopen::GemmDescriptor gemm_desc =
                                    GemmDescriptor{false,
                                                   true,
                                                   false,
                                                   wei_len,
                                                   hy_h,
                                                   (in_n.at(cur_time) - in_n.at(use_time)),
                                                   hy_stride,
                                                   uni_stride,
                                                   uni_stride,
                                                   1, // batch count
                                                   0, // Stride A
                                                   0, // Stride B
                                                   0, // Stride C
                                                   1, // alpha
                                                   1, // beta
                                                   xDesc[0].GetType(),
                                                   false};

                                miopenStatus_t gemm_status = CallGemm(
                                    handle,
                                    gemm_desc,
                                    workSpace,
                                    hid_shift + ri * wei_len + in_n.at(use_time) * hy_stride,
                                    hx,
                                    hx_shift + ri * hy_n * hy_h + in_n.at(use_time) * hy_h,
                                    dw,
                                    wei_shift + ri * wei_len * uni_stride,
                                    GemmBackend_t::miopengemm);

                                if(gemm_status != miopenStatusSuccess)
                                {
                                    if(gemm_status == miopenStatusNotImplemented)
                                    {
                                        MIOPEN_LOG_E("GEMM not implemented");
                                    }
                                    else
                                    {
                                        MIOPEN_LOG_E("GEMM failed");
                                    }
                                }
                                // Update time
                                profileRNNkernels(handle, 1, ctime);
                            }

                            pretime_shift =
                                li * batch_n * hy_stride + pre_batch * hy_stride + hid_off;

                            if(in_n.at(use_time) > 0)
                            {
                                miopen::GemmDescriptor gemm_desc =
                                    GemmDescriptor{false,
                                                   true,
                                                   false,
                                                   wei_len,
                                                   hy_h,
                                                   in_n.at(use_time),
                                                   hy_stride,
                                                   hy_stride,
                                                   uni_stride,
                                                   1, // batch count
                                                   0, // Stride A
                                                   0, // Stride B
                                                   0, // Stride C
                                                   1, // alpha
                                                   1, // beta
                                                   xDesc[0].GetType(),
                                                   false};

                                miopenStatus_t gemm_status =
                                    CallGemm(handle,
                                             gemm_desc,
                                             workSpace,
                                             hid_shift + ri * wei_len,
                                             reserveSpace,
                                             pretime_shift + ri * hy_h,
                                             dw,
                                             wei_shift + ri * wei_len * uni_stride,
                                             GemmBackend_t::miopengemm);

                                if(gemm_status != miopenStatusSuccess)
                                {
                                    if(gemm_status == miopenStatusNotImplemented)
                                    {
                                        MIOPEN_LOG_E("GEMM not implemented");
                                    }
                                    else
                                    {
                                        MIOPEN_LOG_E("GEMM failed");
                                    }
                                }
                                // Update time
                                if(li == nLayers - 1 && ti == seqLen - 1 && ri == bi - 1)
                                    profileRNNkernels(handle, 2, ctime);
                                else
                                    profileRNNkernels(handle, 1, ctime);
                            }
                        }
                    }
                }

                bacc += in_n.at(ti);
            }
        }
    }

#else
    (void)handle;
    (void)seqLen;
    (void)xDesc;
    (void)x;
    (void)hxDesc;
    (void)hx;
    (void)dyDesc;
    (void)dy;
    (void)dwDesc;
    (void)dw;
    (void)workSpace;
    (void)workSpaceSize;
    (void)reserveSpace;
    (void)reserveSpaceSize;
    MIOPEN_THROW("GEMM is not supported");
#endif
};

} // namespace miopen
