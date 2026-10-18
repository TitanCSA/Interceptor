#pragma once
#include <array>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Interceptor::Detail {
// A non-owning, sized range. Containers/arrays/single inputs supply their size
// automatically; pointer+size is only needed when adapting an external buffer.
template<class T> class Input_View {
    T* data_=nullptr;
    std::size_t size_=0;
    template<class U> using Compatible=std::enable_if_t<std::is_convertible_v<U(*)[],T(*)[]>,int>;
public:
    using element_type=T;
    using value_type=std::remove_cv_t<T>;
    constexpr Input_View() noexcept=default;
    Input_View(T* data,std::size_t size):data_(data),size_(size) {
        if(size && !data)throw std::invalid_argument("Input_View: null nonempty buffer");
    }
    constexpr Input_View(T& input) noexcept:data_(&input),size_(1){}
    template<std::size_t N> constexpr Input_View(T (&inputs)[N]) noexcept:data_(inputs),size_(N){}
    template<class U,std::size_t N,Compatible<U> =0>
    constexpr Input_View(std::array<U,N>& inputs) noexcept:data_(inputs.data()),size_(N){}
    template<class U,std::size_t N,Compatible<const U> =0>
    constexpr Input_View(const std::array<U,N>& inputs) noexcept:data_(inputs.data()),size_(N){}
    template<class U,class A,Compatible<U> =0>
    Input_View(std::vector<U,A>& inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    template<class U,class A,Compatible<const U> =0>
    Input_View(const std::vector<U,A>& inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    template<class U,Compatible<U> =0>
    constexpr Input_View(Input_View<U> inputs) noexcept:data_(inputs.data()),size_(inputs.size()){}
    constexpr T* data() const noexcept{return data_;}
    constexpr std::size_t size() const noexcept{return size_;}
    constexpr bool empty() const noexcept{return !size_;}
    constexpr T* begin() const noexcept{return data_;}
    constexpr T* end() const noexcept{return size_?data_+size_:data_;}
    constexpr T& operator[](std::size_t index) const noexcept{return data_[index];}
    Input_View first(std::size_t count) const {
        if(count>size_)throw std::out_of_range("Input_View: prefix exceeds input size");
        return {data_,count};
    }
    Input_View subview(std::size_t offset) const {
        if(offset>size_)throw std::out_of_range("Input_View: offset exceeds input size");
        return {offset?data_+offset:data_,size_-offset};
    }
};
template<class T> Input_View(T&)->Input_View<T>;
template<class T,std::size_t N> Input_View(T (&)[N])->Input_View<T>;
template<class T,std::size_t N> Input_View(std::array<T,N>&)->Input_View<T>;
template<class T,std::size_t N> Input_View(const std::array<T,N>&)->Input_View<const T>;
template<class T,class A> Input_View(std::vector<T,A>&)->Input_View<T>;
template<class T,class A> Input_View(const std::vector<T,A>&)->Input_View<const T>;
}
