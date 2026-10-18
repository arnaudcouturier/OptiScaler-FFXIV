#pragma once

#include <SimpleIni.h>

#include <optional>
#include <string>
#include <utility>

enum HasDefaultValue
{
    WithDefault,
    NoDefault,
    SoftDefault
};

constexpr inline int UnboundKey = -1;

template <class T, HasDefaultValue defaultState = WithDefault> class CustomOptional : public std::optional<T>
{
    using Base = std::optional<T>;

  public:
    CustomOptional(T defaultValue)
        requires(defaultState != NoDefault)
        : Base(), _defaultValue(std::move(defaultValue))
    {
    }

    CustomOptional()
        requires(defaultState == NoDefault)
        : Base(), _defaultValue {}
    {
    }

    // Same behaviour as the real class (Config.h): the value is changed in memory only; what the ini file would get (value_for_config)
    // stays what it was before the first volatile change.
    void set_volatile_value(const T& value)
    {
        if (!_volatile)
        {
            if (this->has_value())
                _configValue = this->value();
            else
                _configValue = std::nullopt;
        }
        _volatile = true;
        Base::operator=(value);
    }

    void set_from_config(const std::optional<T>& value)
    {
        if (!this->has_value())
        {
            _configValue = value;
            Base::operator=(value);
        }
    }

    T value_or_default() const
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? this->value() : _defaultValue;
    }

    CustomOptional& operator=(const T& value)
    {
        _volatile = false;
        Base::operator=(value);
        return *this;
    }

    CustomOptional& operator=(std::optional<T> value)
    {
        _volatile = false;
        Base::operator=(std::move(value));
        return *this;
    }

    std::optional<T> value_for_config()
        requires(defaultState == WithDefault)
    {
        if (_volatile)
            return _configValue != _defaultValue ? _configValue : std::nullopt;
        if (!this->has_value() || *this == _defaultValue)
            return std::nullopt;
        return this->value();
    }

    std::optional<T> value_for_config()
        requires(defaultState != WithDefault)
    {
        return _volatile ? _configValue : static_cast<const Base&>(*this);
    }

    T value_for_config_or(T fallback)
    {
        return value_for_config().value_or(std::move(fallback));
    }

  private:
    T _defaultValue;
    std::optional<T> _configValue;
    bool _volatile = false;
};

enum class FGOutput
{
    NoFG,
    DLSSG,
    XeFG,
    Other
};

class Config
{
  public:
    CustomOptional<::FGOutput> FGOutput { ::FGOutput::DLSSG };

    static Config* Instance()
    {
        static Config value;
        return &value;
    }
};
