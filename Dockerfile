FROM justinhop/buildbase:alpine

RUN mkdir /build /original
#ADD . /original
ADD . /build
WORKDIR /build

RUN apk add  \
    autoconf \
    automake \
    cmake \
    ffmpeg \
    ffmpeg-dev \
    g++ \
    libtool \
    py3-httplib2 \
    py3-pip \
    wget
#    libmicrohttpd-dev \
#    protobuf-dev \

RUN pip3 install --upgrade youtube-dl

#RUN git clone https://github.com/JustinHop/c8tsender.git

RUN cd c8tsender

RUN git clone https://github.com/protocolbuffers/protobuf.git \
        && cd ./protobuf \
        && ./configure \
        && make \
        && make install \
        && cd .. \
        && echo rm -rfv protobuf

RUN wget https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-latest.tar.gz \
        && tar xvaf libmicrohttpd-latest.tar.gz \
        && cd libmicroh* \
        && autoreconf -f -i \
        && ./configure \
        && make \
        && make install \
        && cd .. \
        && echo rm -rfv libmicrohttpd*

