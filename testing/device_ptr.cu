#include <thrusttest/unittest.h>
#include <thrust/device_vector.h>
#include <thrust/device_ptr.h>

void TestDevicePointerManipulation(void)
{
    typedef int T;

    thrust::device_vector<int> data(5);

    thrust::device_ptr<int> begin(&data[0]);
    thrust::device_ptr<int> end(&data[5]);

    ASSERT_EQUAL(end - begin, 5);

    begin++;
    begin--;
    
    ASSERT_EQUAL(end - begin, 5);

    begin += 1;
    begin -= 1;
    
    ASSERT_EQUAL(end - begin, 5);

    begin = begin + (int) 1;
    begin = begin - (int) 1;

    ASSERT_EQUAL(end - begin, 5);

    begin = begin + (unsigned int) 1;
    begin = begin - (unsigned int) 1;
    
    ASSERT_EQUAL(end - begin, 5);
    
    begin = begin + (size_t) 1;
    begin = begin - (size_t) 1;

    ASSERT_EQUAL(end - begin, 5);

    begin = begin + (ptrdiff_t) 1;
    begin = begin - (ptrdiff_t) 1;

    ASSERT_EQUAL(end - begin, 5);

    begin = begin + (thrust::device_ptr<int>::difference_type) 1;
    begin = begin - (thrust::device_ptr<int>::difference_type) 1;

    ASSERT_EQUAL(end - begin, 5);
}
DECLARE_UNITTEST(TestDevicePointerManipulation);

void TestMakeDevicePointer(void)
{
    typedef int T;

    T *raw_ptr = 0;

    thrust::device_ptr<T> p0 = thrust::device_pointer_cast(raw_ptr);

    ASSERT_EQUAL(thrust::raw_pointer_cast(p0), raw_ptr);

    thrust::device_ptr<T> p1 = thrust::device_pointer_cast(p0);

    ASSERT_EQUAL(p0, p1);
}
DECLARE_UNITTEST(TestMakeDevicePointer);

