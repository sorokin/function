#pragma once
#include <exception>
#include <type_traits>

struct bad_function_call : std::exception
{
    char const* what() const noexcept override
    {
        return "bad function call";
    }
};

namespace function_impl
{
    template <typename R, typename... Args>
    struct type_descriptor;

    static size_t const INPLACE_BUFFER_SIZE = sizeof(void*);
    static size_t const INPLACE_BUFFER_ALIGNMENT = alignof(void*);
     
    using inplace_buffer = std::aligned_storage<INPLACE_BUFFER_SIZE, INPLACE_BUFFER_ALIGNMENT>::type;

    /*
    Конъюнкт is_nothrow_move_constructible<T> важен. Дело в том, что move-операции у
    function'а должны быть noexcept, но что делать если move-конструктор у
    small-объекта бросает исключения? Выход: не использовать small-object оптимизацию
    для таких объектов.
    */
    template <typename T>
    static constexpr bool fits_small_storage = sizeof(T) <= INPLACE_BUFFER_SIZE
                                            && alignof(T) <= INPLACE_BUFFER_ALIGNMENT
                                            && std::is_nothrow_move_constructible<T>::value;

    /*
    Просто вспомогательная структурка с акцессорами. Иначе приходится слишком
    много void* и кастов писать.
    */
    template <typename R, typename... Args>
    struct storage
    {
        storage() = default;

        template <typename T>
        T& get_static() noexcept
        {
            return reinterpret_cast<T&>(buf);
        }

        template <typename T>
        T const& get_static() const noexcept
        {
            return reinterpret_cast<T const&>(buf);
        }

        template <typename T>
        T* get_dynamic() const noexcept
        {
            return static_cast<T*>(reinterpret_cast<void* const&>(buf));
        }

        void set_dynamic(void* value) noexcept
        {
            reinterpret_cast<void*&>(buf) = value;
        }

        inplace_buffer buf;
        type_descriptor<R, Args...> const* desc;
    };

    /*
    Чтобы стереть тип, function'у нужно знать, как делать различные операции с внутренним
    объектом: как его копировать, перемещать, вызывать и разрушать.

    Можно было бы в function похранить 4 указателя на функции, но в этом случае function
    занимал бы 4 * sizeof(void*) байт + размер small-буфера. Чтобы сделать function
    компактнее, можно заметить, что эти 4 функции не независимые: если copy соответствует
    компированию типа T, и move будет мувать тип T, invoke вызывать тип T и так далее.

    Поэтому можно вынести эти 4 указателя в отдельную структуру и в function ссылаться на
    эту структуру. В этой реализации такая структура называется type_descriptor. Для каждого
    типа T оборачиваемого в function, нам достаточно иметь один объект type_descriptor,
    соответствующий этому типу. В данной реализации эти объекты будут глобальными переменными.

    То, что получилось в результате очень похоже на таблицу виртуальных функций сгенеренную
    компилятором.
    */
    template <typename R, typename... Args>
    struct type_descriptor
    {
        using storage = function_impl::storage<R, Args...>;

        void (*copy)(storage* dst, storage const* src);
        void (*move)(storage* dst, storage* src) noexcept;
        R (*invoke)(storage const* src, Args...);
        void (*destroy)(storage* src) noexcept;
    };

    template <typename R, typename... Args>
    type_descriptor<R, Args...> const* empty_type_descriptor() noexcept
    {
        using storage = function_impl::storage<R, Args...>;

        /*
        constexpr здесь полезен как способ гарантировать, что у нас будет
        делаться статическая инициализация, а не динамическая.

        Стоит так же обратить внимание, что конверсия лямбд в указатель
        на функцию constexpr лишь начиная с C++17. До C++17 пришлось бы
        объявлять функции и брать их адреса.
        */
        static constexpr type_descriptor<R, Args...> instance =
        {
            /*copy =*/ [] (storage* dst, storage const*)
            {
                dst->desc = empty_type_descriptor<R, Args...>();
            },
            /*move =*/ [] (storage* dst, storage*) noexcept
            {
                dst->desc = empty_type_descriptor<R, Args...>();
            },
            /*invoke =*/ [] (storage const*, Args...) -> R
            {
                throw bad_function_call();
            },
            /*destroy =*/ [] (storage*) noexcept
            {},
        };
        return &instance;
    }

    /*
    В эту структуру вынесены те операции, которые различаются для small и не-small
    объектов.
    */
    template <typename T, typename = void>
    struct object_traits;

    template <typename T>
    struct object_traits<T, std::enable_if_t<fits_small_storage<T>>>
    {
        template <typename R, typename... Args>
        static type_descriptor<R, Args...> const* get_type_descriptor() noexcept
        {
            using storage = storage<R, Args...>;

            static constexpr type_descriptor<R, Args...> instance =
            {
                /*copy =*/ [] (storage* dst, storage const* src)
                {
                    new(&dst->buf) T(src->template get_static<T>());
                    dst->desc = src->desc;
                },
                /*move =*/ [] (storage* dst, storage* src) noexcept
                {
                    new(&dst->buf) T(std::move(src->template get_static<T>()));
                    dst->desc = src->desc;
                },
                /*invoke =*/ [](storage const* src, Args... args) -> R
                {
                    return src->template get_static<T>()(std::forward<Args>(args)...);
                },
                /*destroy =*/ [] (storage* src) noexcept
                {
                    src->template get_static<T>().~T();
                },
            };
            return &instance;
        }

        template <typename R, typename... Args>
        static void initialize_storage(storage<R, Args...>& stg, T&& func_obj)
        {
            new (&stg.buf) T(std::move(func_obj));
        }

        template <typename R, typename... Args>
        static T* as_target(storage<R, Args...>& stg) noexcept
        {
            return &reinterpret_cast<T&>(stg.buf);
        }

        template <typename R, typename... Args>
        static T const* as_target(storage<R, Args...> const& stg) noexcept
        {
            return &reinterpret_cast<T const&>(stg.buf);
        }
    };

    template <typename T>
    struct object_traits<T, std::enable_if_t<!fits_small_storage<T>>>
    {
        template <typename R, typename... Args>
        static type_descriptor<R, Args...> const* get_type_descriptor() noexcept
        {
            using storage = storage<R, Args...>;

            static constexpr type_descriptor<R, Args...> instance =
            {
                /*copy =*/ [] (storage* dst, storage const* src)
                {
                    dst->set_dynamic(new T(*src->template get_dynamic<T>()));
                    dst->desc = src->desc;
                },
                /*move =*/ [] (storage* dst, storage* src) noexcept
                {
                    dst->set_dynamic(src->template get_dynamic<T>());
                    dst->desc = src->desc;
                    src->desc = empty_type_descriptor<R, Args...>();
                },
                /*invoke =*/ [] (storage const* src, Args... args) -> R
                {
                    return (*src->template get_dynamic<T>())(std::forward<Args>(args)...);
                },        
                /*destroy =*/ [] (storage* src) noexcept
                {
                    delete src->template get_dynamic<T>();
                },
            };
            return &instance;
        }

        template <typename R, typename... Args>
        static void initialize_storage(storage<R, Args...>& stg, T&& func_obj)
        {
            stg.set_dynamic(new T(std::move(func_obj)));
        }

        template <typename R, typename... Args>
        static T* as_target(storage<R, Args...>& stg) noexcept
        {
            return stg.template get_dynamic<T>();
        }

        template <typename R, typename... Args>
        static T const* as_target(storage<R, Args...> const& stg) noexcept
        {
            return stg.template get_dynamic<T>();
        }
    };
}
