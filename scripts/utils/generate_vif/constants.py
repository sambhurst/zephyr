#!/usr/bin/env python3

# Copyright (c) 2022 The Chromium OS Authors
# SPDX-License-Identifier: Apache-2.0

XML_ENCODING = "utf-8"
XML_ELEMENT_NAME_PREFIX = "vif"
XML_ROOT_ELEMENT_NAME = "VIF"
XML_NAMESPACE_ATTRIBUTES = {
    "xmlns:opt": "http://usb.org/VendorInfoFileOptionalContent.xsd",
    "xmlns:xsi": "http://www.w3.org/2001/XMLSchema-instance",
    "xmlns:vif": "http://usb.org/VendorInfoFile.xsd",
}

NAME = "name"
VALUE = "value"
TEXT = "text"
ATTRIBUTES = "attributes"
CHILD = "child"
COMPONENT = "Component"

SINK_PDOS = "sink-pdos"
SINK_PDO = "sink-pdo"
SINK_PDO_SUPPLY_TYPE = "Snk_PDO_Supply_Type"
SINK_PDO_VOLTAGE = "Snk_PDO_Voltage"
SINK_PDO_OP_CURRENT = "Snk_PDO_Op_Current"
SINK_PDO_MIN_VOLTAGE = "Snk_PDO_Min_Voltage"
SINK_PDO_MAX_VOLTAGE = "Snk_PDO_Max_Voltage"
SINK_PDO_OP_POWER = "Snk_PDO_Op_Power"
PD_POWER_AS_SINK = "PD_Power_As_Sink"
NUM_SINK_PDOS = "Num_Snk_PDOs"

VIF_SPEC_ELEMENTS = {
    "VIF_Specification": {
        TEXT: "3.19",
    },
    "VIF_App": {
        CHILD: {
            "Vendor": {
                TEXT: "USB-IF",
            },
            "Name": {
                TEXT: "VIF Editor",
            },
            "Version": {
                TEXT: "3.2.4.0",
            }
        }
    },
    "Vendor_Name": {
        TEXT: "Google",
    },
    "VIF_Product_Type": {
        TEXT: "Port Product",
        ATTRIBUTES: {
            "value": "0",
        },
    },
    "Certification_Type": {
        TEXT: "End Product",
        ATTRIBUTES: {
            "value": "0",
        },
    }
}

VIF_ELEMENTS = ["VIF_Specification", "VIF_App", "Vendor", "Name", "Version",
                "Vendor_Name", "VIF_Product_Type", "Certification_Type",
                COMPONENT, SINK_PDOS, SINK_PDO, SINK_PDO_SUPPLY_TYPE,
                SINK_PDO_VOLTAGE, SINK_PDO_OP_CURRENT, SINK_PDO_MIN_VOLTAGE,
                SINK_PDO_MAX_VOLTAGE, SINK_PDO_OP_POWER, PD_POWER_AS_SINK,
                PD_POWER_AS_SINK, NUM_SINK_PDOS]

DT_VIF_ELEMENTS_DICT = {
    SINK_PDOS: "SnkPdoList",
    SINK_PDO: "SnkPDO",
}

PDO_TYPES = {
    0: "Fixed",
    1: "Battery",
    2: "Variable",
    3: "Augmented",
}
