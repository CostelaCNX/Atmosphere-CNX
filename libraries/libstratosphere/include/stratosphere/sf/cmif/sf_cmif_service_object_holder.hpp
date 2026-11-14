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

#pragma once
#include <stratosphere/sf/sf_service_object.hpp>
#include <stratosphere/sf/cmif/sf_cmif_service_dispatch.hpp>

namespace ams::sf::cmif {

    class ServiceObjectHolder {
        private:
            SharedPointer<IServiceObject> m_srv;
            const ServiceDispatchMeta *m_dispatch_meta;
            #if AMS_SF_MITM_SUPPORTED
            /* A returned non-domain MITM object needs its own native forward
             * session. This member is transitional: command serialization
             * moves it into the newly registered ServerSession. */
            std::shared_ptr<::Service> m_forward_service;
            #endif
        private:
            /* Copy constructor. */
            ServiceObjectHolder(const ServiceObjectHolder &o) : m_srv(o.m_srv), m_dispatch_meta(o.m_dispatch_meta)
                #if AMS_SF_MITM_SUPPORTED
                , m_forward_service(o.m_forward_service)
                #endif
            { /* ... */ }
            ServiceObjectHolder &operator=(const ServiceObjectHolder &o) = delete;
        public:
            /* Default constructor, null all members. */
            ServiceObjectHolder() : m_srv(nullptr, false), m_dispatch_meta(nullptr)
                #if AMS_SF_MITM_SUPPORTED
                , m_forward_service()
                #endif
            { /* ... */ }

            ~ServiceObjectHolder() {
                m_dispatch_meta = nullptr;
            }

            /* Ensure correct type id at runtime through template constructor. */
            template<typename ServiceImpl>
            constexpr explicit ServiceObjectHolder(SharedPointer<ServiceImpl> &&s) : m_srv(std::move(s)), m_dispatch_meta(GetServiceDispatchMeta<ServiceImpl>())
                #if AMS_SF_MITM_SUPPORTED
                , m_forward_service()
                #endif
            {
                /* ... */
            }

            #if AMS_SF_MITM_SUPPORTED
            template<typename ServiceImpl>
            constexpr ServiceObjectHolder(SharedPointer<ServiceImpl> &&s, std::shared_ptr<::Service> &&forward_service) : m_srv(std::move(s)), m_dispatch_meta(GetServiceDispatchMeta<ServiceImpl>()), m_forward_service(std::move(forward_service)) {
                static_assert(sf::IsMitmServiceObject<ServiceImpl>);
                AMS_ABORT_UNLESS(m_forward_service != nullptr);
            }
            #endif

            /* Move constructor, assignment operator. */
            ServiceObjectHolder(ServiceObjectHolder &&o) : m_srv(std::move(o.m_srv)), m_dispatch_meta(std::move(o.m_dispatch_meta))
                #if AMS_SF_MITM_SUPPORTED
                , m_forward_service(std::move(o.m_forward_service))
                #endif
            {
                o.m_dispatch_meta = nullptr;
            }

            ServiceObjectHolder &operator=(ServiceObjectHolder &&o) {
                ServiceObjectHolder tmp(std::move(o));
                tmp.swap(*this);
                return *this;
            }

            /* State management. */
            void swap(ServiceObjectHolder &o) {
                m_srv.swap(o.m_srv);
                std::swap(m_dispatch_meta, o.m_dispatch_meta);
                #if AMS_SF_MITM_SUPPORTED
                m_forward_service.swap(o.m_forward_service);
                #endif
            }

            void Reset() {
                m_srv = nullptr;
                m_dispatch_meta = nullptr;
                #if AMS_SF_MITM_SUPPORTED
                m_forward_service.reset();
                #endif
            }

            ServiceObjectHolder Clone() const {
                return ServiceObjectHolder(*this);
            }

            /* Boolean operators. */
            explicit constexpr operator bool() const {
                return m_srv != nullptr;
            }

            constexpr bool operator!() const {
                return m_srv == nullptr;
            }

            /* Getters. */
            constexpr uintptr_t GetServiceId() const {
                if (m_dispatch_meta) {
                    return m_dispatch_meta->GetServiceId();
                }
                return 0;
            }

            template<typename Interface>
            constexpr inline bool IsServiceObjectValid() const {
                return this->GetServiceId() == GetServiceDispatchMeta<Interface>()->GetServiceId();
            }

            template<typename Interface>
            inline Interface *GetServiceObject() const {
                if (this->GetServiceId() == GetServiceDispatchMeta<Interface>()->GetServiceId()) {
                    return static_cast<Interface *>(m_srv.Get());
                }
                return nullptr;
            }

            inline sf::IServiceObject *GetServiceObjectUnsafe() const {
                return static_cast<sf::IServiceObject *>(m_srv.Get());
            }

            #if AMS_SF_MITM_SUPPORTED
            constexpr bool HasForwardService() const {
                return m_forward_service != nullptr;
            }

            std::shared_ptr<::Service> TakeForwardService() {
                return std::move(m_forward_service);
            }

            #endif

            /* Processing. */
            Result ProcessMessage(ServiceDispatchContext &ctx, const cmif::PointerAndSize &in_raw_data) const;
    };

}
