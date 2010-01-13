//
// Copyright 2010 Ettus Research LLC
//

#ifndef INCLUDED_USRP_UHD_DEVICE_ADDR_HPP
#define INCLUDED_USRP_UHD_DEVICE_ADDR_HPP

#include <string>
#include <iostream>
#include <netinet/ether.h>
#include <arpa/inet.h>

namespace usrp_uhd{

    /*!
    * Wrapper for an ethernet mac address.
    * Provides conversion between string and binary formats.
    */
    struct mac_addr_t{
        struct ether_addr mac_addr;
        mac_addr_t(const std::string &str = "00:00:00:00:00:00");
        std::string to_string(void) const;
    };

    /*!
    * Wrapper for an ipv4 address.
    * Provides conversion between string and binary formats.
    */
    struct ip_addr_t{
        struct in_addr ip_addr;
        ip_addr_t(const std::string &str = "0.0.0.0");
        std::string to_string(void) const;
    };

    /*!
    * Possible usrp device interface types.
    */
    enum device_addr_type_t{
        DEVICE_ADDR_TYPE_AUTO,
        DEVICE_ADDR_TYPE_VIRTUAL,
        DEVICE_ADDR_TYPE_USB,
        DEVICE_ADDR_TYPE_ETH,
        DEVICE_ADDR_TYPE_UDP,
        DEVICE_ADDR_TYPE_GPMC
    };

    /*!
    * Structure to hold properties that identify a usrp device.
    */
    struct device_addr_t{
        device_addr_type_t type;
        struct{
            size_t num_rx_dsps;
            size_t num_tx_dsps;
            size_t num_dboards;
        } virtual_args;
        struct{
            uint16_t vendor_id;
            uint16_t product_id;
        } usb_args;
        struct{
            std::string ifc;
            mac_addr_t mac_addr;
        } eth_args;
        struct{
            ip_addr_t ip_addr;
        } udp_args;
        struct{
            //TODO unknown for now
        } gpmc_args;

        /*!
         * \brief Convert a usrp device_addr_t into a string representation
         */
        std::string to_string(void) const;

        /*!
         * \brief Default constructor to initialize the device_addr_t struct
         */
        device_addr_t(device_addr_type_t device_addr_type = DEVICE_ADDR_TYPE_AUTO);
    };

} //namespace usrp_uhd

//ability to use types with stream operators
std::ostream& operator<<(std::ostream &os, const usrp_uhd::device_addr_t &x);
std::ostream& operator<<(std::ostream &os, const usrp_uhd::mac_addr_t &x);
std::ostream& operator<<(std::ostream &os, const usrp_uhd::ip_addr_t &x);

#endif /* INCLUDED_USRP_UHD_DEVICE_ADDR_HPP */