#pragma once
#include "function_impl.h"

template <typename F>
struct function;

template <typename R, typename... Args>
struct function<R (Args...)>
{
    function() noexcept
    {
        stg.desc = function_impl::empty_type_descriptor<R, Args...>();
    }

    /*
    Вся полезная работа делается внутри функций copy/move/etc. Не важно
    храним ли мы small-object, не-small-object или мы пустые.

    Надо просто вызвать соответствующую функцию.
    */
    function(function const& other)
    {
        other.stg.desc->copy(&stg, &other.stg);
    }

    function(function&& other) noexcept
    {
        other.stg.desc->move(&stg, &other.stg);
    }

    template <typename T>
    function(T val)
    {
        using traits = function_impl::object_traits<T>;

        stg.desc = traits::template get_type_descriptor<R, Args...>();
        traits::initialize_storage(stg, std::move(val));
    }

    /*
    Присваивание выглядит жутковато поскольку я попытался удовлетворить
    строгой гарантии безопасности исключений.

    Просто сделать destroy+copy нельзя поскольку copy может бросить и
    мы останемся с разрушенным объектом. Я пользуюсь небросающим move:
    муваю this в backup и потом пытаюсь копировать в this. Если копирование
    бросает -- муваю backup назад.
    */
    function& operator=(function const& rhs)
    {
        if (this == &rhs)
            return *this;

        function_impl::storage<R, Args...> backup;
        stg.desc->move(&backup, &stg);
        stg.desc->destroy(&stg);
        try
        {
            rhs.stg.desc->copy(&stg, &rhs.stg);
        }
        catch (...)
        {
            backup.desc->move(&stg, &backup);
            backup.desc->destroy(&backup);
            throw;
        }
        backup.desc->destroy(&backup);
 
        return *this;
    }
 
    function& operator=(function&& rhs) noexcept
    {
        if (this == &rhs)
            return *this;

        /*
        Так делать безопасно -- move не бросает.
        */
        stg.desc->destroy(&stg);
        rhs.stg.desc->move(&stg, &rhs.stg);
 
        return *this;
    }
 
    ~function()
    {
        stg.desc->destroy(&stg);
    }

    explicit operator bool() const noexcept
    {
        return stg.desc != function_impl::empty_type_descriptor<R, Args...>();
    }

    R operator()(Args... args) const
    {
        return stg.desc->invoke(&stg, std::forward<Args>(args)...);
    }

    template <typename T>
    T* target() noexcept
    {
        using traits = function_impl::object_traits<T>;

        if (stg.desc == traits::template get_type_descriptor<R, Args...>())
            return traits::template as_target(stg);
        else
            return nullptr;
    }

    template <typename T>
    T const* target() const noexcept
    {
        using traits = function_impl::object_traits<T>;

        if (stg.desc == traits::template get_type_descriptor<R, Args...>())
            return traits::template as_target(stg);
        else
            return nullptr;
    }

private:
    function_impl::storage<R, Args...> stg;
};
