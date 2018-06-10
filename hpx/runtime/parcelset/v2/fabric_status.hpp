//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_FABRIC_STATUS_HPP
#define HPX_PARCELSET_V2_FABRIC_STATUS_HPP

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>

#include <cstdlib>
#include <iostream>

namespace hpx { namespace parcelset { inline namespace v2 {
    struct fabric_status
    {
        constexpr fabric_status() : fi_errno(FI_SUCCESS)
        {}

        std::string what() const
        {
            return {fi_strerror(fi_errno)};
        }

        explicit operator bool()
        {
            return *this == success();
        }

        constexpr fabric_status& operator=(int fi_errno_)
        {
            fi_errno = -fi_errno_;
            return *this;
        }

        static constexpr fabric_status success()
        {
            return {FI_SUCCESS};
        }

        /* Operation not permitted */
        static constexpr fabric_status eperm()
        {
         return {FI_EPERM};
        }

        /* No such file or directory */
        static constexpr fabric_status enoent()
        {
         return {FI_ENOENT};
        }

        /* Interrupted system call */
        static constexpr fabric_status eintr()
        {
         return {FI_EINTR};
        }

        /* I/O error */
        static constexpr fabric_status eio()
        {
         return {FI_EIO};
        }

        /* Argument list too long */
        static constexpr fabric_status e2big()
        {
         return {FI_E2BIG};
        }

        /* Bad file number */
        static constexpr fabric_status ebadf()
        {
         return {FI_EBADF};
        }

        /* Try again */
        static constexpr fabric_status eagain()
        {
         return {FI_EAGAIN};
        }

        /* Out of memory */
        static constexpr fabric_status enomem()
        {
         return {FI_ENOMEM};
        }

        /* Permission denied */
        static constexpr fabric_status eacces()
        {
         return {FI_EACCES};
        }

        /* Bad address */
        static constexpr fabric_status efault()
        {
         return {FI_EFAULT};
        }

        /* Device or resource busy */
        static constexpr fabric_status ebusy()
        {
         return {FI_EBUSY};
        }

        /* No such device */
        static constexpr fabric_status enodev()
        {
         return {FI_ENODEV};
        }

        /* Invalid argument */
        static constexpr fabric_status einval()
        {
         return {FI_EINVAL};
        }

        /* Too many open files */
        static constexpr fabric_status emfile()
        {
         return {FI_EMFILE};
        }

        /* No space left on device */
        static constexpr fabric_status enospc()
        {
         return {FI_ENOSPC};
        }

        /* Function not implemented */
        static constexpr fabric_status enosys()
        {
         return {FI_ENOSYS};
        }

        /* Operation would block */
        static constexpr fabric_status ewouldblock()
        {
         return {FI_EWOULDBLOCK};
        }

        /* No message of desired type */
        static constexpr fabric_status enomsg()
        {
         return {FI_ENOMSG};
        }

        /* No data available */
        static constexpr fabric_status enodata()
        {
         return {FI_ENODATA};
        }

        /* Message too long */
        static constexpr fabric_status emsgsize()
        {
         return {FI_EMSGSIZE};
        }

        /* Protocol not available */
        static constexpr fabric_status enoprotoopt()
        {
         return {FI_ENOPROTOOPT};
        }

        /* Operation not supported on transport endpoint */
        static constexpr fabric_status eopnotsupp()
        {
         return {FI_EOPNOTSUPP};
        }

        /* Address already in use */
        static constexpr fabric_status eaddrinuse()
        {
         return {FI_EADDRINUSE};
        }

        /* Cannot assign requested address */
        static constexpr fabric_status eaddrnotavail()
        {
         return {FI_EADDRNOTAVAIL};
        }

        /* Network is down */
        static constexpr fabric_status enetdown()
        {
         return {FI_ENETDOWN};
        }

        /* Network is unreachable */
        static constexpr fabric_status enetunreach()
        {
         return {FI_ENETUNREACH};
        }

        /* Software caused connection abort */
        static constexpr fabric_status econnaborted()
        {
         return {FI_ECONNABORTED};
        }

        /* Connection reset by peer */
        static constexpr fabric_status econnreset()
        {
         return {FI_ECONNRESET};
        }

        /* No buffer space available */
        static constexpr fabric_status enobufs()
        {
         return {FI_ENOBUFS};
        }

        /* Transport endpoint is already connected */
        static constexpr fabric_status eisconn()
        {
         return {FI_EISCONN};
        }

        /* Transport endpoint is not connected */
        static constexpr fabric_status enotconn()
        {
         return {FI_ENOTCONN};
        }

        /* Cannot send after transport endpoint shutdown */
        static constexpr fabric_status eshutdown()
        {
         return {FI_ESHUTDOWN};
        }

        /* Connection timed out */
        static constexpr fabric_status etimedout()
        {
         return {FI_ETIMEDOUT};
        }

        /* Connection refused */
        static constexpr fabric_status econnrefused()
        {
         return {FI_ECONNREFUSED};
        }

        /* Host is down */
        static constexpr fabric_status ehostdown()
        {
         return {FI_EHOSTDOWN};
        }

        /* No route to host */
        static constexpr fabric_status ehostunreach()
        {
         return {FI_EHOSTUNREACH};
        }

        /* Operation already in progress */
        static constexpr fabric_status ealready()
        {
         return {FI_EALREADY};
        }

        /* Operation now in progress */
        static constexpr fabric_status einprogress()
        {
         return {FI_EINPROGRESS};
        }

        /* Remote I/O error */
        static constexpr fabric_status eremoteio()
        {
         return {FI_EREMOTEIO};
        }

        /* Operation Canceled */
        static constexpr fabric_status ecanceled()
        {
         return {FI_ECANCELED};
        }

        /* Key was rejected by service */
        static constexpr fabric_status ekeyrejected()
        {
         return {FI_EKEYREJECTED};
        }

        /* Unspecified error */
        static constexpr fabric_status eother()
        {
         return {FI_EOTHER};
        }

        /* Provided buffer is too small */
        static constexpr fabric_status etoosmall()
        {
         return {FI_ETOOSMALL};
        }

        /* Operation not permitted in current state */
        static constexpr fabric_status eopbadstate()
        {
         return {FI_EOPBADSTATE};
        }

        /* Error available */
        static constexpr fabric_status eavail()
        {
         return {FI_EAVAIL};
        }

        /* Flags not supported */
        static constexpr fabric_status ebadflags()
        {
         return {FI_EBADFLAGS};
        }

        /* Missing or unavailable event queue */
        static constexpr fabric_status enoeq()
        {
         return {FI_ENOEQ};
        }

        /* Invalid resource domain */
        static constexpr fabric_status edomain()
        {
         return {FI_EDOMAIN};
        }

        /* Missing or unavailable completion queue */
        static constexpr fabric_status enocq()
        {
         return {FI_ENOCQ};
        }

        /* CRC error */
        static constexpr fabric_status ecrc()
        {
         return {FI_ECRC};
        }

        /* Truncation error */
        static constexpr fabric_status etrunc()
        {
         return {FI_ETRUNC};
        }

        /* Required key not available */
        static constexpr fabric_status enokey()
        {
         return {FI_ENOKEY};
        }

        /* Missing or unavailable address vector */
        static constexpr fabric_status enoav()
        {
         return {FI_ENOAV};
        }

        /* Queue has been overrun */
        static constexpr fabric_status eoverrun()
        {
         return {FI_EOVERRUN};
        }

    private:
        int fi_errno;

        constexpr fabric_status(int fi_errno_)
          : fi_errno(fi_errno_)
        {}

        friend std::ostream& operator<<(std::ostream& os, fabric_status const& status)
        {
            os << status.what();
            return os;
        }

        friend constexpr bool operator==(fabric_status const& lhs, fabric_status const& rhs)
        {
            return lhs.fi_errno == rhs.fi_errno;
        }

        friend constexpr bool operator!=(fabric_status const& lhs, fabric_status const& rhs)
        {
            return !(lhs == rhs);
        }
    };
}}}

#endif
