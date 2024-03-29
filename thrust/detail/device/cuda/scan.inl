/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


/*! \file scan_dev.inl
 *  \brief Inline file for scan_dev.h.
 */

// do not attempt to compile this file with any other compiler
#ifdef __CUDACC__


#include <thrust/experimental/arch.h>
#include <thrust/functional.h>
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#include <thrust/device_malloc.h>
#include <thrust/device_free.h>
#include <thrust/copy.h>

#include <thrust/scan.h>    //for second level scans


namespace thrust
{

namespace detail
{

namespace device
{

namespace cuda
{

namespace interval_scan
{

//TODO move this to /detail/util/
// ceil(x/y) for integers, used to determine # of blocks/warps etc.
template<typename L, typename R>
  inline __host__ __device__ L divide_into(const L x, const R y)
{
  return (x + y - 1) / y;
}

/////////////
// Kernels //
/////////////

// XXX replace with thrust::detail::warp::scan() after perf testing
template<typename T,
         typename AssociativeOperator>
         __device__
void scan_warp(const unsigned int& thread_lane, volatile T * sdata, const AssociativeOperator op)
{
    // the use of 'volatile' is a workaround so that nvcc doesn't reorder the following lines
    if (thread_lane >=  1)  sdata[threadIdx.x] = op((T &) sdata[threadIdx.x -  1] , (T &) sdata[threadIdx.x]);
    if (thread_lane >=  2)  sdata[threadIdx.x] = op((T &) sdata[threadIdx.x -  2] , (T &) sdata[threadIdx.x]);
    if (thread_lane >=  4)  sdata[threadIdx.x] = op((T &) sdata[threadIdx.x -  4] , (T &) sdata[threadIdx.x]);
    if (thread_lane >=  8)  sdata[threadIdx.x] = op((T &) sdata[threadIdx.x -  8] , (T &) sdata[threadIdx.x]);
    if (thread_lane >= 16)  sdata[threadIdx.x] = op((T &) sdata[threadIdx.x - 16] , (T &) sdata[threadIdx.x]);
}

template<typename OutputType,
         typename AssociativeOperator,
         unsigned int BLOCK_SIZE>
__global__ void
inclusive_update_kernel(OutputType *dst,
                        const unsigned int n,
                        const AssociativeOperator op,
                        const unsigned int num_iters,
                        const unsigned int num_warps,
                        const OutputType * carry_in)
{
  const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;  // global thread index
  const unsigned int thread_lane = threadIdx.x & 31;                       // thread index within the warp
  const unsigned int warp_id     = thread_id   / 32;                       // global warp index

    // 第一个warp在第一个kernel中，已经scan完成，这轮不需要做任何事情
  if(warp_id == 0 || warp_id >= num_warps) return;                         // nothing to do

  const unsigned int begin = warp_id * num_iters * 32 + thread_lane;       // thread offset into array
  const unsigned int end   = min(begin + (num_iters * 32), n);             // end of thread work

  const OutputType carry = carry_in[warp_id - 1];                          // value to add to this segment

    // coalesced access, 32个线程工作在连续的内存，然后stride访问下一组32个元素大小的数组
  for(unsigned int i = begin; i < end; i += 32){
      dst[i] = op(carry, dst[i]);
  }
}

template<typename OutputType,
         typename AssociativeOperator,
         unsigned int BLOCK_SIZE>
__global__ void
exclusive_update_kernel(OutputType *dst,
                        const unsigned int n,
                        const AssociativeOperator op,
                        const unsigned int num_iters,
                        const unsigned int num_warps,
                        const OutputType * carry_in)
{
    __shared__ OutputType sdata[BLOCK_SIZE];
    __shared__ OutputType first[BLOCK_SIZE/32];

    const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;  // global thread index
    const unsigned int thread_lane = threadIdx.x & 31;                       // thread index within the warp
    const unsigned int warp_id     = thread_id   / 32;                       // global warp index
    const unsigned int warp_lane   = threadIdx.x / 32;                       // warp index within the CTA

    if(warp_id >= num_warps) return;                                         // nothing to do

    const unsigned int begin = warp_id * num_iters * 32 + thread_lane;       // thread offset into array
    const unsigned int end   = min(begin + (num_iters * 32), n);             // end of thread work

    const OutputType carry = carry_in[warp_id];                               // value to add to this segment

    if(thread_lane == 0)
        first[warp_lane] = carry;

    for(unsigned int i = begin; i < end; i += 32){
        sdata[threadIdx.x] = op(carry, dst[i]);
        dst[i] = (thread_lane == 0) ? first[warp_lane] : sdata[threadIdx.x - 1];
        if(thread_lane == 31)
            first[warp_lane] = sdata[threadIdx.x];
    }
}

/* Perform an inclusive scan on separate intervals
 *
 * For intervals of length 2:
 *    [ a, b, c, d, ... ] -> [ a, a+b, c, c+d, ... ]
 *
 * Each warp is assigned an interval of src
 */
template<typename InputIterator,
         typename OutputType,
         typename AssociativeOperator,
         unsigned int BLOCK_SIZE>
__global__ void
kernel(InputIterator src,
       const unsigned int n,
       OutputType *dst,
       const AssociativeOperator op,
       const unsigned int interval_size,
       OutputType * final_carry)
{
    // final_carry是全局内存
    // 每个warp处理一些连续warp size的数组时，最后产生的累加和
    // 它会被写入到final carry中对应warp index所在的槽中
    // 接下来第二个kernel启动时，可以被简单对应加到从第二个warp开始处理数据的序列中
    // 从而得到全局的scan结果
  // XXX warpSize exists, but is not known at compile time,
  //     so define our own constant
  const unsigned int WARP_SIZE = 32;

  __shared__ volatile OutputType sdata[BLOCK_SIZE];
  // 一个block有很多warps，每个warp计算结果存入这个shared mem对应的warp位置的元素中
  // 一个warp对应的一个槽
  // 每个warp对应carry是当前warp处理warp size大小数组时产生的scan的最后累加和
  // 它需要被加到下一个warp size数组的所有元素中（代码中是加到第一个元素，因为后面它会scan到那个数组中的所有的元素中）
  __shared__ volatile OutputType carry[BLOCK_SIZE/WARP_SIZE];

  const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;  // global thread index
  const unsigned int thread_lane = threadIdx.x & (WARP_SIZE - 1);          // thread index within the warp
  const unsigned int warp_id     = thread_id   / WARP_SIZE;                // global warp index
  const unsigned int warp_lane   = threadIdx.x / WARP_SIZE;                // warp index within the CTA

    // 当前warp处理的数据的开头
  const unsigned int base = warp_id * interval_size;
  unsigned int i   = base + thread_lane;                                   // initial thread starting position
  unsigned int end;                                                        // end of current sub-segment

  if(base >= n)
      return;

//  /// XXX BEGIN TEST
//  if(thread_lane == 0){
//    end = min(base + interval_size, n);
//
//    OutputType sum = src[i];
//    dst[i] = sum;
//
//    i++;
//    while( i < end ){
//        sum = op(sum, src[i]);
//        dst[i] = sum;
//        i++;
//    }
//    final_carry[warp_id] = sum;
//  }
//  // XXX END TEST

  // First iteration has no carry in
  end = n;
  if(i < n){
    sdata[threadIdx.x] = src[i];

    scan_warp<OutputType>(thread_lane, sdata, op);

    // warp的第一次线程，将warp scan的结果存入carry对应warp的索引位置
    if (thread_lane == 0)
        carry[warp_lane] = sdata[threadIdx.x + min(n - i - 1, WARP_SIZE - 1)];    // since (i + WARP_SIZE - 1) may be >= n

    // 将scan结果存入临时结果中
    dst[i] = sdata[threadIdx.x];

    i += WARP_SIZE;
  }

  // Remaining full-width iterations
  end = min(base + interval_size, n - (n & (WARP_SIZE - 1)));
  while(i < end){
    sdata[threadIdx.x] = src[i];

    if (thread_lane == 0)
        sdata[threadIdx.x] = op((OutputType &) carry[warp_lane], (OutputType &) sdata[threadIdx.x]);

    scan_warp<OutputType>(thread_lane, sdata, op);

    if (thread_lane == (WARP_SIZE - 1))
        carry[warp_lane] = sdata[threadIdx.x];

    dst[i] = sdata[threadIdx.x];

    i += WARP_SIZE;
  }


  // Final non-full-width iteration
  end = min(base + interval_size, n);
  if(i < end){
    sdata[threadIdx.x] = src[i];

    if(thread_lane == 0)
        sdata[threadIdx.x] = op((OutputType &) carry[warp_lane], (OutputType &) sdata[threadIdx.x]);

    scan_warp<OutputType>(thread_lane, sdata, op);

    if(thread_lane == 0)
      carry[warp_lane] = sdata[threadIdx.x + min(n - i - 1, (WARP_SIZE - 1))];

    dst[i] = sdata[threadIdx.x];

    i += WARP_SIZE;
  }

  if(base < end && thread_lane == 0)
    final_carry[warp_id] = carry[warp_lane];
} // end kernel()


} // end namespace interval_scan




//////////////////
// Entry Points //
//////////////////

template<typename InputIterator,
         typename OutputType,
         typename AssociativeOperator>
  void inclusive_scan(InputIterator src,
                      const size_t n,
                      OutputType * dst,
                      AssociativeOperator op)
{
    if( n == 0 )
        return;

    // XXX todo query for warp size
    const unsigned int WARP_SIZE  = 32;
    const unsigned int BLOCK_SIZE = 256;
    // 所有的sm都工作时，一个wave可以启动多少个blocks
    const unsigned int MAX_BLOCKS = experimental::arch::max_active_threads()/BLOCK_SIZE;
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE/WARP_SIZE;  // =8

    // 问题域大小，需要多少个warps同时工作
    const unsigned int num_units  = interval_scan::divide_into(n, WARP_SIZE);
    // 一个wave可以有多少个warps工作
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    // 一个wave可以有多少个blocks工作
    // 如果问题域很大，这个值就是=MAX_BLOCKS
    // 否则，我们不需要那么多的blocks工作
    const unsigned int num_blocks = interval_scan::divide_into(num_warps,WARPS_PER_BLOCK);
    // 如果问题域很大，需要多少轮，也就是多少wave of blocks
    const unsigned int num_iters  = interval_scan::divide_into(num_units, num_warps);          // number of times each warp iterates, interval length is 32*num_iters
    // 每个warp需要处理多少个warp size大小的数组序列
    // 注意一个warp处理的是连续的num_iters个WARP_SIZE大小的数组
    // 第一个kernel只是启动一个wave，每个warp处理num_iters个数的WARP_SIZE大小的数组
    /*
    warp#0: (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) ... (WARP_SIZE)
    warp#1: (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) ... (WARP_SIZE)
    warp#2: (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) ... (WARP_SIZE)
    ...
    warp#n: (WARP_SIZE) (WARP_SIZE) (WARP_SIZE) <<没有元素了>>

    */

    const unsigned int interval_size = WARP_SIZE * num_iters;

    // create a temp vector for per-warp results
    // per-warp的scan，每个warp产生一个局部scan结果
    thrust::device_ptr<OutputType> d_carry_out = thrust::device_malloc<OutputType>(num_warps);

    //////////////////////
    // first level scan
    interval_scan::kernel<InputIterator,OutputType,AssociativeOperator,BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (src, n, dst, op, interval_size, d_carry_out.get());

    bool second_scan_device = true;

    ///////////////////////
    // second level scan
    if (second_scan_device) {
        // scan carry_out on the device (use one warp of GPU method for second level scan)
        interval_scan::kernel<OutputType *,OutputType,AssociativeOperator,WARP_SIZE> <<<1, WARP_SIZE>>>
            (d_carry_out.get(), num_warps, d_carry_out.get(), op, num_warps, (d_carry_out + num_warps - 1).get());
    } else {
        // scan carry_out on the host
        thrust::host_vector<OutputType> h_carry_out(d_carry_out, d_carry_out + num_warps);
        thrust::inclusive_scan(h_carry_out.begin(), h_carry_out.end(), h_carry_out.begin(), op);

        // copy back to device
        thrust::copy(h_carry_out.begin(), h_carry_out.end(), d_carry_out);
    }

    //////////////////////
    // update intervals
    interval_scan::inclusive_update_kernel<OutputType,AssociativeOperator,BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (dst, n, op, num_iters, num_warps, d_carry_out.get());

    // free device work array
    thrust::device_free(d_carry_out);
} // end inclusive_interval_scan()



template<typename InputIterator,
         typename OutputType,
         typename T,
         typename AssociativeOperator>
  void exclusive_scan(InputIterator src,
                      const size_t n,
                      OutputType * dst,
                      const T init,
                      AssociativeOperator op)
{
    if( n == 0 )
        return;

    const unsigned int WARP_SIZE  = 32;
    const unsigned int BLOCK_SIZE = 256;
    const unsigned int MAX_BLOCKS = experimental::arch::max_active_threads()/BLOCK_SIZE;
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE/WARP_SIZE;

    const unsigned int num_units  = interval_scan::divide_into(n, WARP_SIZE);
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    const unsigned int num_blocks = interval_scan::divide_into(num_warps,WARPS_PER_BLOCK);
    const unsigned int num_iters  = interval_scan::divide_into(num_units, num_warps);

    const unsigned int interval_size = WARP_SIZE * num_iters;

    // create a temp vector for per-warp results
    // exclusive scan，多分配一个
    thrust::device_ptr<OutputType> d_carry_out = thrust::device_malloc<OutputType>(num_warps + 1);

    //////////////////////
    // first level scan
    // carry out存储从第1个开始
    interval_scan::kernel<InputIterator,OutputType,AssociativeOperator,BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (src, n, dst, op, interval_size, (d_carry_out + 1).get());

    bool second_scan_device = true;

    ///////////////////////
    // second level scan
    if (second_scan_device) {
        // scan carry_out on the device (use one warp of GPU method for second level scan)
        d_carry_out[0] = init; // set first value
        interval_scan::kernel<OutputType *,OutputType,AssociativeOperator,WARP_SIZE> <<<1, WARP_SIZE>>>
            (d_carry_out.get(), num_warps + 1, d_carry_out.get(), op, num_warps + 1, (d_carry_out + num_warps).get());
    }
    else {
        // scan carry_out on the host
        thrust::host_vector<OutputType> h_carry_out(d_carry_out, d_carry_out + num_warps + 1);
        h_carry_out[0] = init;
        thrust::inclusive_scan(h_carry_out.begin(), h_carry_out.end(), h_carry_out.begin(), op);

        // copy back to device
        thrust::copy(h_carry_out.begin(), h_carry_out.end(), d_carry_out);
    }

    //////////////////////
    // update intervals
    interval_scan::exclusive_update_kernel<OutputType,AssociativeOperator,BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (dst, n, op, num_iters, num_warps, d_carry_out.get());

    // free device work array
    thrust::device_free(d_carry_out);
} // end exclusive_interval_scan()


} // end namespace cuda

} // end namespace device

} // end namespace detail

} // end namespace thrust

#endif // __CUDACC__
