#include "CESLocalGroupPolicyManager.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr std::wstring_view kPoliciesRoot = L"SOFTWARE\\Policies";
constexpr unsigned long kGpUpdateTimeoutMs = 5u * 60u * 1000u;

void SetError(std::wstring const& message, std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::wstring NormalizePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.front() == L'\\') {
        path.erase(path.begin());
    }
    while (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

bool StartsWithCaseInsensitive(std::wstring const& text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        auto left = text[index];
        auto right = prefix[index];
        if (left >= L'a' && left <= L'z') {
            left = static_cast<wchar_t>(left - L'a' + L'A');
        }
        if (right >= L'a' && right <= L'z') {
            right = static_cast<wchar_t>(right - L'a' + L'A');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool IsPolicyRootPath(std::wstring const& path) {
    return StartsWithCaseInsensitive(path, kPoliciesRoot) && path.size() == kPoliciesRoot.size();
}

#ifdef _WIN32
std::wstring FormatWindowsError(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD const length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = L"Windows error " + std::to_wstring(errorCode);
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, buffer + length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
        LocalFree(buffer);
    }
    return message;
}

HKEY ResolveRootKey(CESLocalGroupPolicyManager::Scope scope) {
    return scope == CESLocalGroupPolicyManager::Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

DWORD ResolveRegistryType(CESLocalGroupPolicyManager::ValueType type) {
    switch (type) {
    case CESLocalGroupPolicyManager::ValueType::Dword: return REG_DWORD;
    case CESLocalGroupPolicyManager::ValueType::Qword: return REG_QWORD;
    case CESLocalGroupPolicyManager::ValueType::String: return REG_SZ;
    case CESLocalGroupPolicyManager::ValueType::ExpandString: return REG_EXPAND_SZ;
    case CESLocalGroupPolicyManager::ValueType::MultiString: return REG_MULTI_SZ;
    case CESLocalGroupPolicyManager::ValueType::Binary: return REG_BINARY;
    }
    return REG_NONE;
}

CESLocalGroupPolicyManager::ValueType ResolveValueType(DWORD type) {
    switch (type) {
    case REG_DWORD: return CESLocalGroupPolicyManager::ValueType::Dword;
    case REG_QWORD: return CESLocalGroupPolicyManager::ValueType::Qword;
    case REG_EXPAND_SZ: return CESLocalGroupPolicyManager::ValueType::ExpandString;
    case REG_MULTI_SZ: return CESLocalGroupPolicyManager::ValueType::MultiString;
    case REG_BINARY: return CESLocalGroupPolicyManager::ValueType::Binary;
    case REG_SZ:
    default:
        return CESLocalGroupPolicyManager::ValueType::String;
    }
}

std::vector<wchar_t> BuildMultiStringBuffer(std::vector<std::wstring> const& values) {
    std::size_t totalUnits = 1;
    for (auto const& value : values) {
        totalUnits += value.size() + 1;
    }

    std::vector<wchar_t> buffer(totalUnits, L'\0');
    wchar_t* cursor = buffer.data();
    for (auto const& value : values) {
        std::memcpy(cursor, value.c_str(), value.size() * sizeof(wchar_t));
        cursor += value.size() + 1;
    }
    return buffer;
}

std::vector<std::wstring> ParseMultiString(std::vector<std::uint8_t> const& bytes) {
    std::vector<std::wstring> values;
    if (bytes.empty()) {
        return values;
    }

    auto const* data = reinterpret_cast<wchar_t const*>(bytes.data());
    std::size_t const count = bytes.size() / sizeof(wchar_t);
    std::size_t index = 0;
    while (index < count && data[index] != L'\0') {
        std::size_t end = index;
        while (end < count && data[end] != L'\0') {
            ++end;
        }
        values.emplace_back(data + index, data + end);
        index = end + 1;
    }
    return values;
}

bool QueryValue(
    HKEY key,
    std::wstring const& valueName,
    CESLocalGroupPolicyManager::PolicyValue& value,
    std::wstring* errorMessage) {
    DWORD type = 0;
    DWORD size = 0;
    auto result = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to query registry value size: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    std::vector<std::uint8_t> buffer(size == 0 ? sizeof(wchar_t) : size, 0);
    result = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, buffer.data(), &size);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to read registry value: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    value.type = ResolveValueType(type);
    value.valueName = valueName;
    value.binaryValue.clear();
    value.multiStringValue.clear();
    value.stringValue.clear();
    value.dwordValue = 0;
    value.qwordValue = 0;

    switch (type) {
    case REG_DWORD:
        if (size >= sizeof(DWORD)) {
            value.dwordValue = *reinterpret_cast<DWORD const*>(buffer.data());
        }
        break;
    case REG_QWORD:
        if (size >= sizeof(ULONGLONG)) {
            value.qwordValue = *reinterpret_cast<ULONGLONG const*>(buffer.data());
        }
        break;
    case REG_MULTI_SZ:
        value.multiStringValue = ParseMultiString(buffer);
        break;
    case REG_BINARY:
        value.binaryValue.assign(buffer.begin(), buffer.begin() + size);
        break;
    case REG_SZ:
    case REG_EXPAND_SZ:
    default: {
        auto const* raw = reinterpret_cast<wchar_t const*>(buffer.data());
        std::size_t const length = size / sizeof(wchar_t);
        std::size_t actualLength = length;
        while (actualLength > 0 && raw[actualLength - 1] == L'\0') {
            --actualLength;
        }
        value.stringValue.assign(raw, raw + actualLength);
        break;
    }
    }

    return true;
}

bool RunGpUpdateTarget(wchar_t const* target, std::wstring* errorMessage) {
    wchar_t systemDirectory[MAX_PATH] = {};
    auto const length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        SetError(L"Failed to locate the Windows system directory.", errorMessage);
        return false;
    }

    std::wstring commandLine = L"\"";
    commandLine += systemDirectory;
    commandLine += L"\\gpupdate.exe\" /target:";
    commandLine += target;
    commandLine += L" /force";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
        SetError(L"Failed to launch gpupdate: " + FormatWindowsError(GetLastError()), errorMessage);
        return false;
    }

    auto const waitResult = WaitForSingleObject(processInfo.hProcess, kGpUpdateTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(processInfo.hProcess, ERROR_TIMEOUT);
        }
        auto const waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        if (waitResult == WAIT_TIMEOUT) {
            SetError(L"gpupdate timed out for target " + std::wstring(target) + L".", errorMessage);
        } else {
            SetError(L"Failed while waiting for gpupdate to finish: " + FormatWindowsError(waitError), errorMessage);
        }
        return false;
    }

    DWORD exitCode = 0;
    bool ok = GetExitCodeProcess(processInfo.hProcess, &exitCode) != 0 && exitCode == 0;
    if (!ok) {
        if (exitCode == 0) {
            auto const queryError = GetLastError();
            SetError(L"Failed to query gpupdate exit code: " + FormatWindowsError(queryError), errorMessage);
        } else {
            SetError(L"gpupdate failed for target " + std::wstring(target) + L" with exit code " + std::to_wstring(exitCode) + L".", errorMessage);
        }
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return ok;
}
#endif
} // namespace

std::wstring CESLocalGroupPolicyManager::BuildPolicyKeyPath(std::wstring relativeKeyPath) {
    auto normalized = NormalizePath(std::move(relativeKeyPath));
    if (normalized.empty()) {
        return std::wstring(kPoliciesRoot);
    }
    if (StartsWithCaseInsensitive(normalized, kPoliciesRoot)) {
        return normalized;
    }
    return std::wstring(kPoliciesRoot) + L"\\" + normalized;
}

bool CESLocalGroupPolicyManager::SetPolicyValue(Scope scope, PolicyValue const& value, std::wstring* errorMessage) const {
#ifdef _WIN32
    auto keyPath = BuildPolicyKeyPath(value.keyPath);
    HKEY key = nullptr;
    auto result = RegCreateKeyExW(
        ResolveRootKey(scope),
        keyPath.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to create or open policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    DWORD type = ResolveRegistryType(value.type);
    BYTE const* data = nullptr;
    DWORD size = 0;
    DWORD dwordStorage = value.dwordValue;
    ULONGLONG qwordStorage = value.qwordValue;
    std::wstring stringStorage;
    std::vector<wchar_t> multiStorage;

    switch (value.type) {
    case ValueType::Dword:
        data = reinterpret_cast<BYTE const*>(&dwordStorage);
        size = sizeof(dwordStorage);
        break;
    case ValueType::Qword:
        data = reinterpret_cast<BYTE const*>(&qwordStorage);
        size = sizeof(qwordStorage);
        break;
    case ValueType::String:
    case ValueType::ExpandString:
        stringStorage = value.stringValue;
        data = reinterpret_cast<BYTE const*>(stringStorage.c_str());
        size = static_cast<DWORD>((stringStorage.size() + 1) * sizeof(wchar_t));
        break;
    case ValueType::MultiString:
        multiStorage = BuildMultiStringBuffer(value.multiStringValue);
        data = reinterpret_cast<BYTE const*>(multiStorage.data());
        size = static_cast<DWORD>(multiStorage.size() * sizeof(wchar_t));
        break;
    case ValueType::Binary:
        data = value.binaryValue.empty() ? nullptr : value.binaryValue.data();
        size = static_cast<DWORD>(value.binaryValue.size());
        break;
    }

    result = RegSetValueExW(key, value.valueName.c_str(), 0, type, data, size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to set policy value: " + FormatWindowsError(result), errorMessage);
        return false;
    }
    return true;
#else
    static_cast<void>(scope);
    static_cast<void>(value);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}

bool CESLocalGroupPolicyManager::TryGetPolicyValue(
    Scope scope,
    std::wstring const& relativeKeyPath,
    std::wstring const& valueName,
    PolicyValue& value,
    std::wstring* errorMessage) const {
#ifdef _WIN32
    auto keyPath = BuildPolicyKeyPath(relativeKeyPath);
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(ResolveRootKey(scope), keyPath.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to open policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    value.keyPath = keyPath;
    auto ok = QueryValue(key, valueName, value, errorMessage);
    RegCloseKey(key);
    return ok;
#else
    static_cast<void>(scope);
    static_cast<void>(relativeKeyPath);
    static_cast<void>(valueName);
    static_cast<void>(value);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}

bool CESLocalGroupPolicyManager::DeletePolicyValue(
    Scope scope,
    std::wstring const& relativeKeyPath,
    std::wstring const& valueName,
    std::wstring* errorMessage) const {
#ifdef _WIN32
    auto keyPath = BuildPolicyKeyPath(relativeKeyPath);
    if (IsPolicyRootPath(keyPath)) {
        SetError(L"Refusing to delete values directly from the root policy container.", errorMessage);
        return false;
    }
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(ResolveRootKey(scope), keyPath.c_str(), 0, KEY_SET_VALUE, &key);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to open policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    result = RegDeleteValueW(key, valueName.c_str());
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to delete policy value: " + FormatWindowsError(result), errorMessage);
        return false;
    }
    return true;
#else
    static_cast<void>(scope);
    static_cast<void>(relativeKeyPath);
    static_cast<void>(valueName);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}

bool CESLocalGroupPolicyManager::DeletePolicyKey(Scope scope, std::wstring const& relativeKeyPath, std::wstring* errorMessage) const {
#ifdef _WIN32
    auto keyPath = BuildPolicyKeyPath(relativeKeyPath);
    if (IsPolicyRootPath(keyPath)) {
        SetError(L"Refusing to delete the root policy container.", errorMessage);
        return false;
    }
    auto result = RegDeleteTreeW(ResolveRootKey(scope), keyPath.c_str());
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to delete policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }
    return true;
#else
    static_cast<void>(scope);
    static_cast<void>(relativeKeyPath);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}

bool CESLocalGroupPolicyManager::EnumeratePolicyValues(
    Scope scope,
    std::wstring const& relativeKeyPath,
    std::vector<PolicyValue>& values,
    std::wstring* errorMessage) const {
#ifdef _WIN32
    auto keyPath = BuildPolicyKeyPath(relativeKeyPath);
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(ResolveRootKey(scope), keyPath.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS) {
        SetError(L"Failed to open policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    DWORD valueCount = 0;
    DWORD maxValueNameLength = 0;
    DWORD maxValueDataLength = 0;
    result = RegQueryInfoKeyW(
        key,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &valueCount,
        &maxValueNameLength,
        &maxValueDataLength,
        nullptr,
        nullptr);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(key);
        SetError(L"Failed to inspect policy key: " + FormatWindowsError(result), errorMessage);
        return false;
    }

    std::vector<wchar_t> nameBuffer(maxValueNameLength + 1, L'\0');
    std::vector<std::uint8_t> dataBuffer(maxValueDataLength == 0 ? sizeof(wchar_t) : maxValueDataLength, 0);
    values.clear();

    for (DWORD index = 0; index < valueCount; ++index) {
        DWORD nameLength = static_cast<DWORD>(nameBuffer.size());
        DWORD dataLength = static_cast<DWORD>(dataBuffer.size());
        DWORD type = 0;
        result = RegEnumValueW(
            key,
            index,
            nameBuffer.data(),
            &nameLength,
            nullptr,
            &type,
            dataBuffer.data(),
            &dataLength);
        if (result != ERROR_SUCCESS) {
            RegCloseKey(key);
            SetError(L"Failed to enumerate policy values: " + FormatWindowsError(result), errorMessage);
            return false;
        }

        PolicyValue value;
        value.keyPath = keyPath;
        value.valueName.assign(nameBuffer.data(), nameLength);
        value.type = ResolveValueType(type);

        std::vector<std::uint8_t> raw(dataBuffer.begin(), dataBuffer.begin() + dataLength);
        switch (type) {
        case REG_DWORD:
            if (dataLength >= sizeof(DWORD)) {
                value.dwordValue = *reinterpret_cast<DWORD const*>(raw.data());
            }
            break;
        case REG_QWORD:
            if (dataLength >= sizeof(ULONGLONG)) {
                value.qwordValue = *reinterpret_cast<ULONGLONG const*>(raw.data());
            }
            break;
        case REG_MULTI_SZ:
            value.multiStringValue = ParseMultiString(raw);
            break;
        case REG_BINARY:
            value.binaryValue = std::move(raw);
            break;
        case REG_SZ:
        case REG_EXPAND_SZ:
        default: {
            auto const* text = reinterpret_cast<wchar_t const*>(raw.data());
            std::size_t textLength = dataLength / sizeof(wchar_t);
            while (textLength > 0 && text[textLength - 1] == L'\0') {
                --textLength;
            }
            value.stringValue.assign(text, text + textLength);
            break;
        }
        }

        values.push_back(std::move(value));
    }

    RegCloseKey(key);
    return true;
#else
    static_cast<void>(scope);
    static_cast<void>(relativeKeyPath);
    static_cast<void>(values);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}

bool CESLocalGroupPolicyManager::ApplyPolicies(bool updateMachine, bool updateUser, std::wstring* errorMessage) const {
#ifdef _WIN32
    if (!updateMachine && !updateUser) {
        return true;
    }
    if (updateMachine && !RunGpUpdateTarget(L"computer", errorMessage)) {
        return false;
    }
    if (updateUser && !RunGpUpdateTarget(L"user", errorMessage)) {
        return false;
    }
    return true;
#else
    static_cast<void>(updateMachine);
    static_cast<void>(updateUser);
    SetError(L"CESLocalGroupPolicyManager is only supported on Windows.", errorMessage);
    return false;
#endif
}
