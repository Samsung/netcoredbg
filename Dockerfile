FROM ubuntu:20.04 AS builder-stage

ARG DEBIAN_FRONTEND=noninteractive

# Copy contents to opt
COPY . /opt/netcoredbg

# Installing packages
RUN apt update && apt install -y curl git cmake clang g++-aarch64-linux-gnu && \
    rm -rf /var/lib/apt/lists/*

# set libraries and fix path for aarch64
ENV CPLUS_INCLUDE_PATH=/usr/aarch64-linux-gnu/include/c++/9/aarch64-linux-gnu
RUN cd /usr/include && ln -s ../aarch64-linux-gnu/include aarch64-linux-gnu

# download everything and compile
RUN cd /usr/local/bin && curl -fsSL https://dot.net/v1/dotnet-install.sh > dotnet-install.sh && chmod 755 /usr/local/bin/dotnet-install.sh && dotnet-install.sh --channel 3.1 --install-dir /opt/dotnet && dotnet-install.sh --architecture arm64 --channel 3.1 --install-dir /opt/dotnet-aarch64
RUN cd /opt && git clone -b release/3.1 https://github.com/dotnet/coreclr.git
RUN DBGSHIM_LOCATION=`find /opt/dotnet-aarch64/shared/Microsoft.NETCore.App \( -name dbgshim.dll -o -name libdbgshim.so -o -name libdbgshim.dylib \) | head -n 1` && \
    mkdir -p /opt/netcoredbg/build && cd /opt/netcoredbg/build && \
    CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin/netcoredbg -DCORECLR_DIR=/opt/coreclr -DDOTNET_DIR=/opt/dotnet -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu -DDBGSHIM_LOCATION=${DBGSHIM_LOCATION} && make && make install && \ 
    cd /opt/netcoredbg/bin && tar -zcvf netcoredbg-linux-arm64.tar.gz netcoredbg

FROM scratch
COPY --from=builder-stage /opt/netcoredbg/bin/netcoredbg-linux-arm64.tar.gz /