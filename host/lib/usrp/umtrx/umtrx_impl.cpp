//
// Copyright 2012 Fairwaves
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "umtrx_impl.hpp"
#include "../usrp2/fw_common.h"
#include "apply_corrections.hpp"
#include <uhd/utils/log.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/exception.hpp>
#include <uhd/transport/if_addrs.hpp>
#include <uhd/transport/udp_zero_copy.hpp>
#include <uhd/types/ranges.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/byteswap.hpp>
#include <uhd/utils/safe_call.hpp>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio.hpp> //used for htonl and ntohl
#include <boost/math/special_functions/round.hpp>
#include <boost/math/special_functions/sign.hpp>
#include "validate_subdev_spec.hpp"
#include <uhd/usrp/dboard_iface.hpp>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::transport;
namespace asio = boost::asio;

/***********************************************************************
 * Make
 **********************************************************************/
static device::sptr umtrx_make(const device_addr_t &device_addr){
    return device::sptr(new umtrx_impl(device_addr));
}

static device_addrs_t umtrx_find(const device_addr_t &hint_) {
    return usrp2_find_generic(hint_, (char *)"umtrx", UMTRX_CTRL_ID_REQUEST, UMTRX_CTRL_ID_RESPONSE);
}

UHD_STATIC_BLOCK(register_umtrx_device){
    device::register_device(&umtrx_find, &umtrx_make);
}

/***********************************************************************
 * Helpers
 **********************************************************************/

static zero_copy_if::sptr make_xport(
    const std::string &addr,
    const std::string &port,
    const device_addr_t &hints,
    const std::string &filter
){

    //only copy hints that contain the filter word
    device_addr_t filtered_hints;
    BOOST_FOREACH(const std::string &key, hints.keys()){
        if (key.find(filter) == std::string::npos) continue;
        filtered_hints[key] = hints[key];
    }

    //make the transport object with the filtered hints
    zero_copy_if::sptr xport = udp_zero_copy::make(addr, port, filtered_hints);

    //Send a small data packet so the umtrx knows the udp source port.
    //This setup must happen before further initialization occurs
    //or the async update packets will cause ICMP destination unreachable.
    static const boost::uint32_t data[2] = {
        uhd::htonx(boost::uint32_t(0 /* don't care seq num */)),
        uhd::htonx(boost::uint32_t(USRP2_INVALID_VRT_HEADER))
    };
    transport::managed_send_buffer::sptr send_buff = xport->get_send_buff();
    std::memcpy(send_buff->cast<void*>(), &data, sizeof(data));
    send_buff->commit(sizeof(data));

    return xport;
}

/***********************************************************************
 * Structors
 **********************************************************************/
umtrx_impl::umtrx_impl(const device_addr_t &_device_addr){
    UHD_MSG(status) << "Opening a UmTRX device..." << std::endl;
    device_addr_t device_addr = _device_addr;
    //setup the dsp transport hints (default to a large recv buff)
    if (not device_addr.has_key("recv_buff_size")){
        #if defined(UHD_PLATFORM_MACOS) || defined(UHD_PLATFORM_BSD)
            //limit buffer resize on macos or it will error
            device_addr["recv_buff_size"] = "1e6";
        #elif defined(UHD_PLATFORM_LINUX) || defined(UHD_PLATFORM_WIN32)
            //set to half-a-second of buffering at max rate
            device_addr["recv_buff_size"] = "50e6";
        #endif
    }
    if (not device_addr.has_key("send_buff_size")){
        //The buffer should be the size of the SRAM on the device,
        //because we will never commit more than the SRAM can hold.
        device_addr["send_buff_size"] = boost::lexical_cast<std::string>(USRP2_SRAM_BYTES);
    }

    device_addrs_t device_args = separate_device_addr(device_addr);

    //extract the user's requested MTU size or default
    mtu_result_t user_mtu;
    user_mtu.recv_mtu = size_t(device_addr.cast<double>("recv_frame_size", udp_simple::mtu));
    user_mtu.send_mtu = size_t(device_addr.cast<double>("send_frame_size", udp_simple::mtu));

    try{
        //calculate the minimum send and recv mtu of all devices
        mtu_result_t mtu = determine_mtu(device_args[0]["addr"], user_mtu);
        for (size_t i = 1; i < device_args.size(); i++){
            mtu_result_t mtu_i = determine_mtu(device_args[i]["addr"], user_mtu);
            mtu.recv_mtu = std::min(mtu.recv_mtu, mtu_i.recv_mtu);
            mtu.send_mtu = std::min(mtu.send_mtu, mtu_i.send_mtu);
        }

        device_addr["recv_frame_size"] = boost::lexical_cast<std::string>(mtu.recv_mtu);
        device_addr["send_frame_size"] = boost::lexical_cast<std::string>(mtu.send_mtu);

        UHD_MSG(status) << boost::format("Current recv frame size: %d bytes") % mtu.recv_mtu << std::endl;
        UHD_MSG(status) << boost::format("Current send frame size: %d bytes") % mtu.send_mtu << std::endl;
    }
    catch(const uhd::not_implemented_error &){
        //just ignore this error, makes older fw work...
    }

    device_args = separate_device_addr(device_addr); //update args for new frame sizes

    ////////////////////////////////////////////////////////////////////
    // create controller objects and initialize the properties tree
    ////////////////////////////////////////////////////////////////////
    _tree = property_tree::make();
    _tree->create<std::string>("/name").set("UmTRX Device");

    for (size_t mbi = 0; mbi < device_args.size(); mbi++){
        const device_addr_t device_args_i = device_args[mbi];
        const std::string mb = boost::lexical_cast<std::string>(mbi);
        const std::string addr = device_args_i["addr"];
        const fs_path mb_path = "/mboards/" + mb;

        ////////////////////////////////////////////////////////////////
        // create the iface that controls i2c, spi, uart, and wb
        ////////////////////////////////////////////////////////////////
        _mbc[mb].iface = usrp2_iface::make(udp_simple::make_connected(
            addr, BOOST_STRINGIZE(USRP2_UDP_CTRL_PORT)
        ));
        _tree->create<std::string>(mb_path / "name").set(_mbc[mb].iface->get_cname());
        _tree->create<std::string>(mb_path / "fw_version").set(_mbc[mb].iface->get_fw_version_string());

        //check the fpga compatibility number
        const boost::uint32_t fpga_compat_num = _mbc[mb].iface->peek32(U2_REG_COMPAT_NUM_RB);
        boost::uint16_t fpga_major = fpga_compat_num >> 16, fpga_minor = fpga_compat_num & 0xffff;
        if (fpga_major == 0){ //old version scheme
            fpga_major = fpga_minor;
            fpga_minor = 0;
        }
        if (fpga_major != USRP2_FPGA_COMPAT_NUM){
            throw uhd::runtime_error(str(boost::format(
                "\nPlease update the firmware and FPGA images for your device.\n"
                "See the application notes for UmTRX for instructions.\n"
                "Expected FPGA compatibility number %d, but got %d:\n"
                "The FPGA build is not compatible with the host code build."
            ) % int(USRP2_FPGA_COMPAT_NUM) % fpga_major));
        }
        _tree->create<std::string>(mb_path / "fpga_version").set(str(boost::format("%u.%u") % fpga_major % fpga_minor));

        //lock the device/motherboard to this process
        _mbc[mb].iface->lock_device(true);

        ////////////////////////////////////////////////////////////////
        // construct transports for RX and TX DSPs
        ////////////////////////////////////////////////////////////////
        UHD_LOG << "Making transport for RX DSP0..." << std::endl;
        _mbc[mb].rx_dsp_xports.push_back(make_xport(
            addr, BOOST_STRINGIZE(USRP2_UDP_RX_DSP0_PORT), device_args_i, "recv"
        ));
        UHD_LOG << "Making transport for RX DSP1..." << std::endl;
        _mbc[mb].rx_dsp_xports.push_back(make_xport(
            addr, BOOST_STRINGIZE(USRP2_UDP_RX_DSP1_PORT), device_args_i, "recv"
        ));
        UHD_LOG << "Making transport for TX DSP0..." << std::endl;
        _mbc[mb].tx_dsp_xport = make_xport(
            addr, BOOST_STRINGIZE(USRP2_UDP_TX_DSP0_PORT), device_args_i, "send"
        );
        //set the filter on the router to take dsp data from this port
        _mbc[mb].iface->poke32(U2_REG_ROUTER_CTRL_PORTS, USRP2_UDP_TX_DSP0_PORT);

        ////////////////////////////////////////////////////////////////
        // setup the mboard eeprom
        ////////////////////////////////////////////////////////////////
        _tree->create<mboard_eeprom_t>(mb_path / "eeprom")
            .set(_mbc[mb].iface->mb_eeprom)
            .subscribe(boost::bind(&umtrx_impl::set_mb_eeprom, this, mb, _1));
/*
        ////////////////////////////////////////////////////////////////
        // create clock control objects
        ////////////////////////////////////////////////////////////////
        _mbc[mb].clock = umtrx_clock_ctrl::make(_mbc[mb].iface);
        _tree->create<double>(mb_path / "tick_rate")
            .publish(boost::bind(&umtrx_clock_ctrl::get_master_clock_rate, _mbc[mb].clock))
            .subscribe(boost::bind(&umtrx_impl::update_tick_rate, this, _1));
*/
        ////////////////////////////////////////////////////////////////
        // create codec control objects
        ////////////////////////////////////////////////////////////////
        const fs_path rx_codec_path = mb_path / "rx_codecs/A";
        const fs_path tx_codec_path = mb_path / "tx_codecs/A";
        _tree->create<int>(rx_codec_path / "gains"); //phony property so this dir exists
        _tree->create<int>(tx_codec_path / "gains"); //phony property so this dir exists
/*        _mbc[mb].codec = umtrx_codec_ctrl::make(_mbc[mb].iface);
        switch(_mbc[mb].iface->get_rev()){
        case usrp2_iface::USRP_N200:
        case usrp2_iface::USRP_N210:
        case usrp2_iface::USRP_N200_R4:
        case usrp2_iface::USRP_N210_R4:{
            _tree->create<std::string>(rx_codec_path / "name").set("ads62p44");
            _tree->create<meta_range_t>(rx_codec_path / "gains/digital/range").set(meta_range_t(0, 6.0, 0.5));
            _tree->create<double>(rx_codec_path / "gains/digital/value")
                .subscribe(boost::bind(&umtrx_codec_ctrl::set_rx_digital_gain, _mbc[mb].codec, _1)).set(0);
            _tree->create<meta_range_t>(rx_codec_path / "gains/fine/range").set(meta_range_t(0, 0.5, 0.05));
            _tree->create<double>(rx_codec_path / "gains/fine/value")
                .subscribe(boost::bind(&umtrx_codec_ctrl::set_rx_digital_fine_gain, _mbc[mb].codec, _1)).set(0);
        }break;

        case usrp2_iface::umtrx_REV3:
        case usrp2_iface::umtrx_REV4:
            _tree->create<std::string>(rx_codec_path / "name").set("ltc2284");
            break;
        case usrp2_iface::USRP_NXXX:
            _tree->create<std::string>(rx_codec_path / "name").set("??????");
            break;
        }
        _tree->create<std::string>(tx_codec_path / "name").set("ad9777");
*/
_tree->create<std::string>(tx_codec_path / "name").set("LMS_TX");
_tree->create<std::string>(rx_codec_path / "name").set("LMS_RX");
        ////////////////////////////////////////////////////////////////
        // create gpsdo control objects
        ////////////////////////////////////////////////////////////////
        if (_mbc[mb].iface->mb_eeprom["gpsdo"] == "internal"){
            _mbc[mb].gps = gps_ctrl::make(udp_simple::make_uart(udp_simple::make_connected(
                addr, BOOST_STRINGIZE(umtrx_UDP_UART_GPS_PORT)
            )));
            if(_mbc[mb].gps->gps_detected()) {
                BOOST_FOREACH(const std::string &name, _mbc[mb].gps->get_sensors()){
                    _tree->create<sensor_value_t>(mb_path / "sensors" / name)
                        .publish(boost::bind(&gps_ctrl::get_sensor, _mbc[mb].gps, name));
                }
            }
        }
/*
        ////////////////////////////////////////////////////////////////
        // and do the misc mboard sensors
        ////////////////////////////////////////////////////////////////
        _tree->create<sensor_value_t>(mb_path / "sensors/mimo_locked")
            .publish(boost::bind(&umtrx_impl::get_mimo_locked, this, mb));
        _tree->create<sensor_value_t>(mb_path / "sensors/ref_locked")
            .publish(boost::bind(&umtrx_impl::get_ref_locked, this, mb));
*/
        ////////////////////////////////////////////////////////////////
        // create frontend control objects
        ////////////////////////////////////////////////////////////////
        _mbc[mb].rx_fe = rx_frontend_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_RX_FRONT)
        );
        _mbc[mb].tx_fe = tx_frontend_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_TX_FRONT)
        );

        _tree->create<subdev_spec_t>(mb_path / "rx_subdev_spec")
            .subscribe(boost::bind(&umtrx_impl::update_rx_subdev_spec, this, mb, _1));
        _tree->create<subdev_spec_t>(mb_path / "tx_subdev_spec")
            .subscribe(boost::bind(&umtrx_impl::update_tx_subdev_spec, this, mb, _1));

        const fs_path rx_fe_path = mb_path / "rx_frontends" / "A";
        const fs_path tx_fe_path = mb_path / "tx_frontends" / "A";

        _tree->create<std::complex<double> >(rx_fe_path / "dc_offset" / "value")
            .coerce(boost::bind(&rx_frontend_core_200::set_dc_offset, _mbc[mb].rx_fe, _1))
            .set(std::complex<double>(0.0, 0.0));
        _tree->create<bool>(rx_fe_path / "dc_offset" / "enable")
            .subscribe(boost::bind(&rx_frontend_core_200::set_dc_offset_auto, _mbc[mb].rx_fe, _1))
            .set(true);
        _tree->create<std::complex<double> >(rx_fe_path / "iq_balance" / "value")
            .subscribe(boost::bind(&rx_frontend_core_200::set_iq_balance, _mbc[mb].rx_fe, _1))
            .set(std::polar<double>(1.0, 0.0));
        _tree->create<std::complex<double> >(tx_fe_path / "dc_offset" / "value")
            .coerce(boost::bind(&tx_frontend_core_200::set_dc_offset, _mbc[mb].tx_fe, _1))
            .set(std::complex<double>(0.0, 0.0));
        _tree->create<std::complex<double> >(tx_fe_path / "iq_balance" / "value")
            .subscribe(boost::bind(&tx_frontend_core_200::set_iq_balance, _mbc[mb].tx_fe, _1))
            .set(std::polar<double>(1.0, 0.0));

        ////////////////////////////////////////////////////////////////
        // create rx dsp control objects
        ////////////////////////////////////////////////////////////////
        _mbc[mb].rx_dsps.push_back(rx_dsp_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_RX_DSP0), U2_REG_SR_ADDR(SR_RX_CTRL0), USRP2_RX_SID_BASE + 0, true
        ));
        _mbc[mb].rx_dsps.push_back(rx_dsp_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_RX_DSP1), U2_REG_SR_ADDR(SR_RX_CTRL1), USRP2_RX_SID_BASE + 1, true
        ));
        for (size_t dspno = 0; dspno < _mbc[mb].rx_dsps.size(); dspno++){
            _mbc[mb].rx_dsps[dspno]->set_link_rate(USRP2_LINK_RATE_BPS);
//            _tree->access<double>(mb_path / "tick_rate")
  //              .subscribe(boost::bind(&rx_dsp_core_200::set_tick_rate, _mbc[mb].rx_dsps[dspno], _1));
            fs_path rx_dsp_path = mb_path / str(boost::format("rx_dsps/%u") % dspno);
            _tree->create<meta_range_t>(rx_dsp_path / "rate/range")
                .publish(boost::bind(&rx_dsp_core_200::get_host_rates, _mbc[mb].rx_dsps[dspno]));
/*
            _tree->create<double>(rx_dsp_path / "rate/value")
                .set(1e6) //some default
                .coerce(boost::bind(&rx_dsp_core_200::set_host_rate, _mbc[mb].rx_dsps[dspno], _1))
                .subscribe(boost::bind(&umtrx_impl::update_rx_samp_rate, this, mb, dspno, _1));
*/
            _tree->create<double>(rx_dsp_path / "freq/value")
                .coerce(boost::bind(&rx_dsp_core_200::set_freq, _mbc[mb].rx_dsps[dspno], _1));
            _tree->create<meta_range_t>(rx_dsp_path / "freq/range")
                .publish(boost::bind(&rx_dsp_core_200::get_freq_range, _mbc[mb].rx_dsps[dspno]));
            _tree->create<stream_cmd_t>(rx_dsp_path / "stream_cmd")
                .subscribe(boost::bind(&rx_dsp_core_200::issue_stream_command, _mbc[mb].rx_dsps[dspno], _1));
        }

        ////////////////////////////////////////////////////////////////
        // create tx dsp control objects
        ////////////////////////////////////////////////////////////////
        _mbc[mb].tx_dsp = tx_dsp_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_TX_DSP), U2_REG_SR_ADDR(SR_TX_CTRL), USRP2_TX_ASYNC_SID
        );
        _mbc[mb].tx_dsp->set_link_rate(USRP2_LINK_RATE_BPS);
//        _tree->access<double>(mb_path / "tick_rate")
  //          .subscribe(boost::bind(&tx_dsp_core_200::set_tick_rate, _mbc[mb].tx_dsp, _1));
        _tree->create<meta_range_t>(mb_path / "tx_dsps/0/rate/range")
            .publish(boost::bind(&tx_dsp_core_200::get_host_rates, _mbc[mb].tx_dsp));
/*  
        _tree->create<double>(mb_path / "tx_dsps/0/rate/value")
            .set(1e6) //some default
            .coerce(boost::bind(&tx_dsp_core_200::set_host_rate, _mbc[mb].tx_dsp, _1))
            .subscribe(boost::bind(&umtrx_impl::update_tx_samp_rate, this, mb, 0, _1));
  
        _tree->create<double>(mb_path / "tx_dsps/0/freq/value")
            .coerce(boost::bind(&umtrx_impl::set_tx_dsp_freq, this, mb, _1));
        _tree->create<meta_range_t>(mb_path / "tx_dsps/0/freq/range")
            .publish(boost::bind(&umtrx_impl::get_tx_dsp_freq_range, this, mb));
*/
        //setup dsp flow control
        const double ups_per_sec = device_args_i.cast<double>("ups_per_sec", 20);
        const size_t send_frame_size = _mbc[mb].tx_dsp_xport->get_send_frame_size();
        const double ups_per_fifo = device_args_i.cast<double>("ups_per_fifo", 8.0);
        _mbc[mb].tx_dsp->set_updates(
            (ups_per_sec > 0.0)? size_t(100e6/*approx tick rate*//ups_per_sec) : 0,
            (ups_per_fifo > 0.0)? size_t(USRP2_SRAM_BYTES/ups_per_fifo/send_frame_size) : 0
        );

        ////////////////////////////////////////////////////////////////
        // create time control objects
        ////////////////////////////////////////////////////////////////
        time64_core_200::readback_bases_type time64_rb_bases;
        time64_rb_bases.rb_secs_now = U2_REG_TIME64_SECS_RB_IMM;
        time64_rb_bases.rb_ticks_now = U2_REG_TIME64_TICKS_RB_IMM;
        time64_rb_bases.rb_secs_pps = U2_REG_TIME64_SECS_RB_PPS;
        time64_rb_bases.rb_ticks_pps = U2_REG_TIME64_TICKS_RB_PPS;
        _mbc[mb].time64 = time64_core_200::make(
            _mbc[mb].iface, U2_REG_SR_ADDR(SR_TIME64), time64_rb_bases, mimo_clock_sync_delay_cycles
        );
//        _tree->access<double>(mb_path / "tick_rate")
  //          .subscribe(boost::bind(&time64_core_200::set_tick_rate, _mbc[mb].time64, _1));
        _tree->create<time_spec_t>(mb_path / "time/now")
            .publish(boost::bind(&time64_core_200::get_time_now, _mbc[mb].time64))
            .subscribe(boost::bind(&time64_core_200::set_time_now, _mbc[mb].time64, _1));
        _tree->create<time_spec_t>(mb_path / "time/pps")
            .publish(boost::bind(&time64_core_200::get_time_last_pps, _mbc[mb].time64))
            .subscribe(boost::bind(&time64_core_200::set_time_next_pps, _mbc[mb].time64, _1));
        //setup time source props
        _tree->create<std::string>(mb_path / "time_source/value")
            .subscribe(boost::bind(&time64_core_200::set_time_source, _mbc[mb].time64, _1));
        _tree->create<std::vector<std::string> >(mb_path / "time_source/options")
            .publish(boost::bind(&time64_core_200::get_time_sources, _mbc[mb].time64));
        //setup reference source props
  //      _tree->create<std::string>(mb_path / "clock_source/value")
//            .subscribe(boost::bind(&umtrx_impl::update_clock_source, this, mb, _1));
        static const std::vector<std::string> clock_sources = boost::assign::list_of("internal")("external")("mimo");
        _tree->create<std::vector<std::string> >(mb_path / "clock_source/options").set(clock_sources);

        ////////////////////////////////////////////////////////////////
        // create dboard control objects
        ////////////////////////////////////////////////////////////////

        //read the dboard eeprom to extract the dboard ids
        dboard_eeprom_t rx_db_eeprom, tx_db_eeprom, gdb_eeprom;
        rx_db_eeprom.load(*_mbc[mb].iface, USRP2_I2C_ADDR_RX_DB);
        tx_db_eeprom.load(*_mbc[mb].iface, USRP2_I2C_ADDR_TX_DB);
        gdb_eeprom.load(*_mbc[mb].iface, USRP2_I2C_ADDR_TX_DB ^ 5);
/*
        //create the properties and register subscribers
        _tree->create<dboard_eeprom_t>(mb_path / "dboards/A/rx_eeprom")
            .set(rx_db_eeprom)
            .subscribe(boost::bind(&umtrx_impl::set_db_eeprom, this, mb, "rx", _1));
        _tree->create<dboard_eeprom_t>(mb_path / "dboards/A/tx_eeprom")
            .set(tx_db_eeprom)
            .subscribe(boost::bind(&umtrx_impl::set_db_eeprom, this, mb, "tx", _1));
        _tree->create<dboard_eeprom_t>(mb_path / "dboards/A/gdb_eeprom")
            .set(gdb_eeprom)
            .subscribe(boost::bind(&umtrx_impl::set_db_eeprom, this, mb, "gdb", _1));
*/
        //create a new dboard interface and manager
            _mbc[mb].dboard_iface = make_umtrx_dboard_iface(_mbc[mb].iface);
            
        _tree->create<dboard_iface::sptr>(mb_path / "dboards/A/iface").set(_mbc[mb].dboard_iface);
        _mbc[mb].dboard_manager = dboard_manager::make(
            rx_db_eeprom.id, tx_db_eeprom.id, gdb_eeprom.id,
            _mbc[mb].dboard_iface, _tree->subtree(mb_path / "dboards/A")
        );
/*
        //bind frontend corrections to the dboard freq props
        const fs_path db_tx_fe_path = mb_path / "dboards" / "A" / "tx_frontends";
        BOOST_FOREACH(const std::string &name, _tree->list(db_tx_fe_path)){
            _tree->access<double>(db_tx_fe_path / name / "freq" / "value")
                .subscribe(boost::bind(&umtrx_impl::set_tx_fe_corrections, this, mb, _1));
        }
        const fs_path db_rx_fe_path = mb_path / "dboards" / "A" / "rx_frontends";
        BOOST_FOREACH(const std::string &name, _tree->list(db_rx_fe_path)){
            _tree->access<double>(db_rx_fe_path / name / "freq" / "value")
                .subscribe(boost::bind(&umtrx_impl::set_rx_fe_corrections, this, mb, _1));
        }
*/
    }

 // internal debug routines
//    this->io_init();
//reg_dump();
//do some post-init tasks
    BOOST_FOREACH(const std::string &mb, _mbc.keys()){
        fs_path root = "/mboards/" + mb;

        _tree->access<subdev_spec_t>(root / "rx_subdev_spec").set(subdev_spec_t("A:" + _tree->list(root / "dboards/A/rx_frontends").at(0)));
        _tree->access<subdev_spec_t>(root / "tx_subdev_spec").set(subdev_spec_t("A:" + _tree->list(root / "dboards/A/tx_frontends").at(0)));
//        _tree->access<std::string>(root / "clock_source/value").set("internal");
        _tree->access<std::string>(root / "time_source/value").set("none");

        //GPS installed: use external ref, time, and init time spec
        if (_mbc[mb].gps.get() and _mbc[mb].gps->gps_detected()){
            UHD_MSG(status) << "Setting references to the internal GPSDO" << std::endl;
            _tree->access<std::string>(root / "time_source/value").set("external");
//            _tree->access<std::string>(root / "clock_source/value").set("external");
            UHD_MSG(status) << "Initializing time to the internal GPSDO" << std::endl;
            _mbc[mb].time64->set_time_next_pps(time_spec_t(time_t(_mbc[mb].gps->get_sensor("gps_time").to_int()+1)));
        }
    }

}

umtrx_impl::~umtrx_impl(void){UHD_SAFE_CALL(
    BOOST_FOREACH(const std::string &mb, _mbc.keys()){
        _mbc[mb].tx_dsp->set_updates(0, 0);
    }
)}

void umtrx_impl::set_mb_eeprom(const std::string &mb, const uhd::usrp::mboard_eeprom_t &mb_eeprom){
    mb_eeprom.commit(*(_mbc[mb].iface), mboard_eeprom_t::MAP_N100);
}
/*
void umtrx_impl::set_db_eeprom(const std::string &mb, const std::string &type, const uhd::usrp::dboard_eeprom_t &db_eeprom){
    if (type == "rx") db_eeprom.store(*_mbc[mb].iface, umtrx_I2C_ADDR_RX_DB);
    if (type == "tx") db_eeprom.store(*_mbc[mb].iface, umtrx_I2C_ADDR_TX_DB);
    if (type == "gdb") db_eeprom.store(*_mbc[mb].iface, umtrx_I2C_ADDR_TX_DB ^ 5);
}

sensor_value_t umtrx_impl::get_mimo_locked(const std::string &mb){
    const bool lock = (_mbc[mb].iface->peek32(U2_REG_IRQ_RB) & (1<<10)) != 0;
    return sensor_value_t("MIMO", lock, "locked", "unlocked");
}

sensor_value_t umtrx_impl::get_ref_locked(const std::string &mb){
    const bool lock = (_mbc[mb].iface->peek32(U2_REG_IRQ_RB) & (1<<11)) != 0;
    return sensor_value_t("Ref", lock, "locked", "unlocked");
}

void umtrx_impl::set_rx_fe_corrections(const std::string &mb, const double lo_freq){
    apply_rx_fe_corrections(this->get_tree()->subtree("/mboards/" + mb), "A", lo_freq);
}

void umtrx_impl::set_tx_fe_corrections(const std::string &mb, const double lo_freq){
    apply_tx_fe_corrections(this->get_tree()->subtree("/mboards/" + mb), "A", lo_freq);
}


double umtrx_impl::set_tx_dsp_freq(const std::string &mb, const double freq_){
    double new_freq = freq_;
    const double tick_rate = _tree->access<double>("/mboards/"+mb+"/tick_rate").get();

    //calculate the DAC shift (multiples of rate)
    const int sign = boost::math::sign(new_freq);
    const int zone = std::min(boost::math::iround(new_freq/tick_rate), 2);
    const double dac_shift = sign*zone*tick_rate;
    new_freq -= dac_shift; //update FPGA DSP target freq

    //set the DAC shift (modulation mode)
    if (zone == 0) _mbc[mb].codec->set_tx_mod_mode(0); //no shift
    else _mbc[mb].codec->set_tx_mod_mode(sign*4/zone); //DAC interp = 4

    return _mbc[mb].tx_dsp->set_freq(new_freq) + dac_shift; //actual freq
}

meta_range_t umtrx_impl::get_tx_dsp_freq_range(const std::string &mb){
    const double tick_rate = _tree->access<double>("/mboards/"+mb+"/tick_rate").get();
    const meta_range_t dsp_range = _mbc[mb].tx_dsp->get_freq_range();
    return meta_range_t(dsp_range.start() - tick_rate*2, dsp_range.stop() + tick_rate*2, dsp_range.step());
}
*/
void umtrx_impl::update_rx_subdev_spec(const std::string &which_mb, const subdev_spec_t &spec){
    fs_path root = "/mboards/" + which_mb + "/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "rx", which_mb);

    //setup mux for this spec
    bool fe_swapped = false;
    for (size_t i = 0; i < spec.size(); i++){
        const std::string conn = _tree->access<std::string>(root / spec[i].db_name / "rx_frontends" / spec[i].sd_name / "connection").get();
        if (i == 0 and (conn == "QI" or conn == "Q")) fe_swapped = true;
        _mbc[which_mb].rx_dsps[i]->set_mux(conn, fe_swapped);
    }
    _mbc[which_mb].rx_fe->set_mux(fe_swapped);

    //compute the new occupancy and resize
    _mbc[which_mb].rx_chan_occ = spec.size();
    size_t nchan = 0;
    BOOST_FOREACH(const std::string &mb, _mbc.keys()) nchan += _mbc[mb].rx_chan_occ;
}

void umtrx_impl::update_tx_subdev_spec(const std::string &which_mb, const subdev_spec_t &spec){
    fs_path root = "/mboards/" + which_mb + "/dboards";

    //sanity checking
    validate_subdev_spec(_tree, spec, "tx", which_mb);

    //set the mux for this spec
    const std::string conn = _tree->access<std::string>(root / spec[0].db_name / "tx_frontends" / spec[0].sd_name / "connection").get();
    _mbc[which_mb].tx_fe->set_mux(conn);

    //compute the new occupancy and resize
    _mbc[which_mb].tx_chan_occ = spec.size();
    size_t nchan = 0;
    BOOST_FOREACH(const std::string &mb, _mbc.keys()) nchan += _mbc[mb].tx_chan_occ;
}

/***********************************************************************
 * Receive streamer
 **********************************************************************/
rx_streamer::sptr umtrx_impl::get_rx_stream(const uhd::stream_args_t &) {
    rx_streamer::sptr my_streamer;
    return my_streamer;
}

uhd::tx_streamer::sptr umtrx_impl::get_tx_stream(const uhd::stream_args_t &) { 
    uhd::tx_streamer::sptr FIXME; return FIXME; 
}

bool umtrx_impl::recv_async_msg(uhd::async_metadata_t &, double) { 
    return false; 
} 



// spi_config_t::EDGE_RISE is used by default
uint32_t umtrx_impl::read_addr(uint8_t lms, uint8_t addr, bool rise) {
    if(addr > 127) return 0; // incorrect address, 7 bit long expected
    if(rise) {
	BOOST_FOREACH(const std::string &mb, _mbc.keys()) {// EVIL HACK - ignore everything after 1st call
	    return _mbc[mb].iface->read_spi(lms, spi_config_t::EDGE_RISE, addr << 8, 16);
	}
    }
    BOOST_FOREACH(const std::string &mb, _mbc.keys()) {// EVIL HACK - ignore everything after 1st call
        return _mbc[mb].iface->read_spi(lms, spi_config_t::EDGE_FALL, addr << 8, 16);
    }
    return 0; // placeholder for error handling
}

uint32_t umtrx_impl::write_n_check(uint8_t lms, uint8_t addr, uint8_t data, bool rise) {
    write_addr(lms, addr, data, rise);
    return read_addr(lms, addr, rise);
}

void umtrx_impl::write_addr(uint8_t lms, uint8_t addr, uint8_t data, bool rise) {
    if(addr < 128) { // 1st bit is 1 (means 'write'), than address, than value
        uint16_t command = (((uint16_t)0x80 | (uint16_t)addr) << 8) | (uint16_t)data;
        if(rise) { 
    	    BOOST_FOREACH(const std::string &mb, _mbc.keys()) {// EVIL HACK - write into all possible places
		_mbc[mb].iface->write_spi(lms, spi_config_t::EDGE_RISE, command, 16);
	    }
    	}
        else { 
    	    BOOST_FOREACH(const std::string &mb, _mbc.keys()) {// EVIL HACK - write into all possible places
    		_mbc[mb].iface->write_spi(lms, spi_config_t::EDGE_FALL, command, 16); 
    	    }
    	}
    }
}

void umtrx_impl::reg_dump(bool rise) {
    for (int i = 0; i < 128; i++) {
        printf("i=%x LMS1=%x LMS2=%x\t", i, read_addr(1, i, rise), read_addr(2, i, rise));
	if(read_addr(1, i, rise) == read_addr(2, i, rise)) printf("OK\n"); else printf("DIFF\n");
    }
}