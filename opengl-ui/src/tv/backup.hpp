// ProsperoTV - Portable settings backups and redacted failure reports.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ptv
{
enum class StorageAction
{
    none,
    backup,
    restore,
    report
};
struct StorageRequest
{
    StorageAction action = StorageAction::none;
    std::string drive;
};
std::vector<std::string> usb_drives(const std::string &mount_root = "/mnt");
// The caller closes its model and database before either operation. A rollback journal
// restores the previous files if the process exits partway through a restore.
bool backup_settings(const std::string &directory, const std::string &archive, std::string &error);
bool restore_settings(const std::string &directory, const std::string &archive, std::string &error);
bool recover_settings(const std::string &directory, std::string &error);
bool export_failure_report(const std::string &logs, const std::string &target, std::string &error);
std::string failure_report(std::string_view receipt, std::string_view version,
                           std::uint64_t timestamp, int result, unsigned attempts);
} // namespace ptv
