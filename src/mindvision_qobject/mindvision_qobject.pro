TEMPLATE = subdirs

SUBDIRS += \
    mindvision_lib \
    camera_gui \
    camera_server \
    mindvision_py

mindvision_lib.file = mindvision_lib.pro
camera_gui.file = camera_gui.pro
camera_server.file = camera_server.pro
mindvision_py.file = mindvision_py.pro

camera_gui.depends = mindvision_lib
camera_server.depends = mindvision_lib
mindvision_py.depends = mindvision_lib
