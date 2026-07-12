// MSVC 17.14+ STL vectorization linker mismatch workaround stubs
#if defined(_MSC_VER) && _MSC_VER < 1940
#include <cstdint>
#include <cstddef>

extern "C" {
    size_t __stdcall __std_find_first_not_of_trivial_pos_1(
        const void* _Haystack, size_t _Haystack_length, const void* _Needle, size_t _Needle_length) noexcept 
    {
        const char* haystack = static_cast<const char*>(_Haystack);
        const char* needle = static_cast<const char*>(_Needle);
        for (size_t i = 0; i < _Haystack_length; ++i) {
            bool found = false;
            for (size_t j = 0; j < _Needle_length; ++j) {
                if (haystack[i] == needle[j]) {
                    found = true;
                    break;
                }
            }
            if (!found) return i;
        }
        return _Haystack_length;
    }

    const void* __stdcall __std_min_element_8i(const void* _First, const void* _Last) noexcept {
        const int64_t* first = static_cast<const int64_t*>(_First);
        const int64_t* last = static_cast<const int64_t*>(_Last);
        if (first == last) return first;
        const int64_t* min_el = first;
        while (++first != last) {
            if (*first < *min_el) {
                min_el = first;
            }
        }
        return min_el;
    }

    const void* __stdcall __std_max_element_d_(const void* _First, const void* _Last) noexcept {
        const double* first = static_cast<const double*>(_First);
        const double* last = static_cast<const double*>(_Last);
        if (first == last) return first;
        const double* max_el = first;
        while (++first != last) {
            if (*max_el < *first) {
                max_el = first;
            }
        }
        return max_el;
    }
}
#endif
