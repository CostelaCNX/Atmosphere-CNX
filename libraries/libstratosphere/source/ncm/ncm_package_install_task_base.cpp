/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>

namespace ams::ncm {

    Result PackageInstallTaskBase::Initialize(const char *package_root_path, void *buffer, size_t buffer_size, StorageId storage_id, InstallTaskDataBase *data, u32 config) {
        R_TRY(InstallTaskBase::Initialize(storage_id, data, config));
        m_package_root.Assign(package_root_path);
        m_buffer = buffer;
        m_buffer_size = buffer_size;
        R_SUCCEED();
    }

    Result PackageInstallTaskBase::OnWritePlaceHolder(const ContentMetaKey &key, InstallContentInfo *content_info) {
        AMS_UNUSED(key);

        /* Get the file path. */
        PackagePath path;
        if (content_info->GetType() == ContentType::Meta) {
            this->CreateContentMetaPath(std::addressof(path), content_info->GetId());
        } else {
            this->CreateContentPath(std::addressof(path), content_info->GetId());
        }

        /* Open the file. */
        fs::FileHandle file;
        R_TRY(fs::OpenFile(std::addressof(file), path, fs::OpenMode_Read));
        ON_SCOPE_EXIT { fs::CloseFile(file); };

        /* Continuously write the file to the placeholder until there is nothing left to write. */
        while (true) {
            /* Read as much of the remainder of the file as possible. */
            size_t size_read;
            R_TRY(fs::ReadFile(std::addressof(size_read), file, content_info->written, m_buffer, m_buffer_size));

            /* There is nothing left to read. */
            if (size_read == 0) {
                break;
            }

            /* Write the placeholder. */
            R_TRY(this->WritePlaceHolderBuffer(content_info, m_buffer, size_read));
        }

        R_SUCCEED();
    }

    Result PackageInstallTaskBase::InstallTicket(const fs::RightsId &rights_id, ContentMetaType meta_type) {
        AMS_UNUSED(meta_type);

        /* Read ticket from file. */
        s64 ticket_size;
        std::unique_ptr<char[]> ticket;
        {
            fs::FileHandle ticket_file;
            {
                PackagePath ticket_path;
                this->CreateTicketPath(std::addressof(ticket_path), rights_id);
                R_TRY_CATCH(fs::OpenFile(std::addressof(ticket_file), ticket_path, fs::OpenMode_Read)) {
                    /* Packages without a ticket are valid for content that
                     * already has a ticket installed.  Preserve NCM's
                     * existing ignorable-ticket behavior in that case. */
                    R_CATCH(fs::ResultPathNotFound) {
                        R_THROW(ncm::ResultIgnorableInstallTicketFailure());
                    }
                } R_END_TRY_CATCH;
            }
            ON_SCOPE_EXIT { fs::CloseFile(ticket_file); };

            R_TRY(fs::GetFileSize(std::addressof(ticket_size), ticket_file));

            R_UNLESS(ticket_size > 0, ncm::ResultIgnorableInstallTicketFailure());
            ticket.reset(new (std::nothrow) char[static_cast<size_t>(ticket_size)]);
            R_UNLESS(ticket != nullptr, ncm::ResultAllocationFailed());
            R_TRY(fs::ReadFile(ticket_file, 0, ticket.get(), static_cast<size_t>(ticket_size)));
        }


        /* Read certificate from file. */
        s64 cert_size;
        std::unique_ptr<char[]> cert;
        {
            fs::FileHandle cert_file;
            {
                PackagePath cert_path;
                this->CreateCertificatePath(std::addressof(cert_path), rights_id);
                R_TRY_CATCH(fs::OpenFile(std::addressof(cert_file), cert_path, fs::OpenMode_Read)) {
                    R_CATCH(fs::ResultPathNotFound) {
                        R_THROW(ncm::ResultIgnorableInstallTicketFailure());
                    }
                } R_END_TRY_CATCH;
            }
            ON_SCOPE_EXIT { fs::CloseFile(cert_file); };

            R_TRY(fs::GetFileSize(std::addressof(cert_size), cert_file));

            R_UNLESS(cert_size > 0, ncm::ResultIgnorableInstallTicketFailure());
            cert.reset(new (std::nothrow) char[static_cast<size_t>(cert_size)]);
            R_UNLESS(cert != nullptr, ncm::ResultAllocationFailed());
            R_TRY(fs::ReadFile(cert_file, 0, cert.get(), static_cast<size_t>(cert_size)));
        }

        /* Import the ticket into Horizon's ES database.  This is the same ES
         * operation used by user-space installers, but is performed here at
         * the NCM commit boundary so content routed to Host gets the same
         * rights treatment as content written to SD or NAND. */
        os::NativeHandle service_handle;
        R_TRY(sm::GetServiceHandle(std::addressof(service_handle), sm::ServiceName::Encode("es")));

        Service es_service{};
        serviceCreate(std::addressof(es_service), service_handle);
        ON_SCOPE_EXIT { serviceClose(std::addressof(es_service)); };

        const Result ticket_import_result = serviceDispatch(std::addressof(es_service), 1,
            .buffer_attrs = {
                SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            },
            .buffers = {
                { ticket.get(), static_cast<size_t>(ticket_size) },
                { cert.get(), static_cast<size_t>(cert_size) },
            }
        );
        /* Keep the result visible even when the installer was launched with
         * the USB route disabled. Ticket import is an ES operation and the
         * counters are diagnostic; gating them on the route used to make a
         * missing/failed ticket look identical to a package with no ticket. */
        ncm::RecordUsbTicketImport(ticket_import_result);
        R_RETURN(ticket_import_result);

        R_SUCCEED();
    }

    void PackageInstallTaskBase::CreateContentPath(PackagePath *out_path, ContentId content_id) {
        char str[ContentIdStringLength + 1] = {};
        GetStringFromContentId(str, sizeof(str), content_id);
        out_path->AssignFormat("%s%s%s", m_package_root.Get(), str, ".nca");
    }

    void PackageInstallTaskBase::CreateContentMetaPath(PackagePath *out_path, ContentId content_id) {
        char str[ContentIdStringLength + 1] = {};
        GetStringFromContentId(str, sizeof(str), content_id);
        out_path->AssignFormat("%s%s%s", m_package_root.Get(), str, ".cnmt.nca");
    }

    void PackageInstallTaskBase::CreateTicketPath(PackagePath *out_path, fs::RightsId id) {
        char str[RightsIdStringLength + 1] = {};
        GetStringFromRightsId(str, sizeof(str), id);
        out_path->AssignFormat("%s%s%s", m_package_root.Get(), str, ".tik");
    }

    void PackageInstallTaskBase::CreateCertificatePath(PackagePath *out_path, fs::RightsId id) {
        char str[RightsIdStringLength + 1] = {};
        GetStringFromRightsId(str, sizeof(str), id);
        out_path->AssignFormat("%s%s%s", m_package_root.Get(), str, ".cert");
    }

}
