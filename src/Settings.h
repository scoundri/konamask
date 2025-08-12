#pragma once
#include <string>
#include <unordered_map>

class Settings {
public:
    int Initialize();
    static Settings& GetInstance();
    bool LoadFromFile(const std::string& filename);
    bool SaveToFile(const std::string& filename) const;
    static bool CopyFile(const std::string& src, const std::string& dest);
    // templated getters for retrieving values with type conversion
    template<typename T>
    T get(const std::string& key, const T& defaultValue = T()) const;

    // templated setters for modifying at runtime
    template<typename T>
    void set(const std::string& key, const T& value);

private:
    Settings() = default;
    ~Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    std::unordered_map<std::string, std::string> data_;
};