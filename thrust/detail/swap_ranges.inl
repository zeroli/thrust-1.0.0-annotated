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


/*! \file swap_ranges.inl
 *  \brief Inline file for swap_ranges.h.
 */

#include <thrust/swap_ranges.h>
#include <thrust/iterator/iterator_categories.h>
#include <thrust/detail/dispatch/swap_ranges.h>

namespace thrust
{

template<typename ForwardIterator1,
         typename ForwardIterator2>
  ForwardIterator2 swap_ranges(ForwardIterator1 first1,
                               ForwardIterator1 last1,
                               ForwardIterator2 first2)
{
  return thrust::detail::dispatch::swap_ranges(first1, last1, first2,
    typename thrust::iterator_traits<ForwardIterator1>::iterator_category(),
    typename thrust::iterator_traits<ForwardIterator2>::iterator_category());
} // end swap_ranges()

} // end namespace thrust

