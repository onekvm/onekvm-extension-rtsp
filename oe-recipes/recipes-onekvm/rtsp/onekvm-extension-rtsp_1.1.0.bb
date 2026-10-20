SUMMARY = "OneKVM RTSP protocol extension"
HOMEPAGE = "https://github.com/onekvm/onekvm-extension-rtsp"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464"
PR = "r2"

ONEKVM_COMPONENT_GIT_URI ??= "git://github.com/onekvm/onekvm-extension-rtsp.git;protocol=https;branch=main"
ONEKVM_COMPONENT_SRCREV ??= ""
SRC_URI = "${ONEKVM_COMPONENT_GIT_URI}"
SRCREV = "${ONEKVM_COMPONENT_SRCREV}"

inherit cmake pkgconfig onekvm-extension

DEPENDS = "json-c live555 openssl"

ONEKVM_EXTENSION_ID = "rtsp"
ONEKVM_EXTENSION_API_VERSION = "2"
ONEKVM_EXTENSION_MANIFEST_TEMPLATE = "${S}/manifest.json.in"
RDEPENDS:${PN} += "onekvm-core (>= 0.1.0-r1)"

EXTRA_OECMAKE += " \
    -DCMAKE_INSTALL_PREFIX=${ONEKVM_EXTENSION_ROOT} \
    -DONEKVM_EXTENSION_VERSION=${PV} \
    -DONEKVM_EXTENSION_ARCH=${TARGET_ARCH} \
    -DONEKVM_BUILD_TESTS=ON \
"
