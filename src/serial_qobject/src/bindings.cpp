#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include "SerialWorker.h"

namespace py = pybind11;

PYBIND11_MODULE(_serial_qobject_py, m) {
    py::class_<SerialWorker>(m, "SerialWorker")
        .def(py::init<>())
        .def("setRemoteMode", &SerialWorker::setRemoteMode)
        .def("isRemoteMode", &SerialWorker::isRemoteMode)
        .def("setRemoteEndpoint", &SerialWorker::setRemoteEndpoint,
             py::arg("host"), py::arg("port") = 8766)
        .def("remoteHost", &SerialWorker::remoteHost)
        .def("remotePort", &SerialWorker::remotePort)
        .def("available_ports", [](SerialWorker &self) {
            return self.available_ports();
        })
        .def("is_connected", &SerialWorker::is_connected)
        .def("current_port_name", &SerialWorker::current_port_name)
        .def("register_log_callback", &SerialWorker::register_log_callback)
        .def("register_status_callback", &SerialWorker::register_status_callback)
        .def("connect_serial", &SerialWorker::connect_serial)
        .def("disconnect_serial", &SerialWorker::disconnect_serial)
        .def("send_command", &SerialWorker::send_command)
        .def("send_raw_command", &SerialWorker::send_raw_command)
        .def("poll_serial", &SerialWorker::poll_serial);
}