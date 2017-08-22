Name:      netcoredbg
Summary:   PUT SUMMARY HERE
Version:   1.0.0
Release:   1
Group:     Development/Toolchain
License:   MIT
Source0:   netcoredbg.tar.gz
Source1001: netcoredbg.manifest

BuildRequires: cmake
BuildRequires: clang >= 3.8
BuildRequires: clang-devel >= 3.8
BuildRequires: llvm >= 3.8
BuildRequires: llvm-devel >= 3.8
BuildRequires: lldb >= 3.8
BuildRequires: lldb-devel >= 3.8
BuildRequires: libstdc++-devel
BuildRequires: coreclr-devel
Requires: coreclr



# .NET Core Runtime
%define dotnet_version  2.0.0
%define dotnetdir       dotnet
%define netshareddir    %{dotnetdir}/shared
%define netcoreappdir   %{netshareddir}/Microsoft.NETCore.App/%{dotnet_version}
%define sdktoolsdir     /home/owner/share/tmp/sdk_tools

%ifarch x86_64
%define ARCH AMD64
%endif

%ifarch armv7l
%define ARCH ARM
%endif

%ifarch %{ix86}
%define ARCH I386
%endif

%ifarch aarch64
%define ARCH ARM64
%endif

%description
This is a CoreCLR debugger for Tizen.

%prep
gzip -dc %{SOURCE0} | tar -xvf -
cd netcoredbg
cp %{SOURCE1001} ..
%build

export GCC_INSTALL_DIR=$(gcc -print-search-dirs | sed -ne '/install: /s/install: //p')
export LLVM_LIBDIR=$(llvm-config --libdir)
export LLVM_INCLUDEDIR=$(llvm-config --includedir)
export GPP_INCLUDE_PATHS=$(cpp -xc++ -v < /dev/null 2>&1 | \
          awk '/search starts here:/{flag=1;next}/End of search list/{flag=0}flag' | \
          sed -e "s/^ //" | tr '\n' ':' | sed -e 's/.$//')
export CLANG_INCLUDE_PATHS=$(clang++ -E -xc++ - -v < /dev/null 2>&1 | \
          awk '/search starts here:/{flag=1;next}/End of search list/{flag=0}flag' | \
          sed -e "s/^ //" | tr '\n' ':' | sed -e 's/.$//')
export CLANG_HEADERS=$(clang++ -print-search-dirs 2>&1 | sed -ne '/libraries: /s/libraries: =//p' | sed -ne 's|:.*|/include|p')

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${LLVM_LIBDIR}:${GCC_INSTALL_DIR}
export LIBRARY_PATH=$LIBRARY_PATH:${LLVM_LIBDIR}:${GCC_INSTALL_DIR}
export CFLAGS=" -B${LLVM_LIBDIR} -B${GCC_INSTALL_DIR} -Wno-deprecated-declarations"
export CPPFLAGS=" -B${LLVM_LIBDIR} -B${GCC_INSTALL_DIR} -Wno-deprecated-declarations"
export CXXFLAGS=" -B${LLVM_LIBDIR} -B${GCC_INSTALL_DIR} -Wno-deprecated-declarations"
export CPLUS_INCLUDE_PATH="${LLVM_INCLUDEDIR}/llvm/:${LLVM_INCLUDEDIR}/llvm-c/:${CLANG_HEADERS}:${GPP_INCLUDE_PATHS}:${CLANG_INCLUDE_PATHS}"
export C_INCLUDE_PATH="${LLVM_INCLUDEDIR}/llvm-c/:%{_includedir}"

mkdir build
cd build
cmake ../netcoredbg \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCLR_BIN_DIR=%{_datarootdir}/%{netcoreappdir} \
    -DCLR_DIR=%{_datarootdir}/%{netcoreappdir} \
    -DINSTALL_DIR=%{_datarootdir}/%{netcoreappdir} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCLR_CMAKE_TARGET_ARCH_%{ARCH}=1 \
    -DBUILD_MANAGED=OFF
make %{?jobs:-j%jobs}

%install
cd build
%make_install

%files
%manifest netcoredbg.manifest
%{_datarootdir}/%{netcoreappdir}/netcoredbg
