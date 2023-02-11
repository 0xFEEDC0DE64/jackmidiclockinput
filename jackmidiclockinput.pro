QT =
CONFIG += c++latest
LIBS += -ljack -lfmt
DEFINES += \
    RING_BUFFER_CONSTEXPR \
    RING_BUFFER_NOEXCEPT \
    RING_BUFFER_CONSTEXPR_DESTRUCTORS
INCLUDEPATH += cxx-ring-buffer/include
HEADERS += \
    cxx-ring-buffer/include/ring-buffer-config.h \
    cxx-ring-buffer/include/ring-buffer-iterator.h \
    cxx-ring-buffer/include/ring-buffer.h
SOURCES += main.cpp
