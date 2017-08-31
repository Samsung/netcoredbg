Name:      netcoredbg
Summary:   Managed code debugger for CoreCLR
Version:   1.0.0
Release:   1
Group:     Development/Toolchain
License:   MIT
Source0:   netcoredbg.tar.gz
Source1001: netcoredbg.manifest
Patch0:     SOS-rename.patch
AutoReqProv: no

BuildRequires: cmake
BuildRequires: clang >= 3.8
BuildRequires: clang-devel >= 3.8
BuildRequires: llvm >= 3.8
BuildRequires: llvm-devel >= 3.8
BuildRequires: lldb >= 3.8
BuildRequires: lldb-devel >= 3.8
BuildRequires: libstdc++-devel
BuildRequires: coreclr-devel
BuildRequires: dotnet-build-tools
Requires: coreclr

# .NET Core Runtime
%define dotnet_version  2.0.0
%define dotnetdir       dotnet
%define netshareddir    %{dotnetdir}/shared
%define netcoreappdir   %{netshareddir}/Microsoft.NETCore.App/%{dotnet_version}
%define sdktoolsdir     /home/owner/share/tmp/sdk_tools
%define install_prefix /usr/
%define sdk_install_prefix /home/owner/share/tmp/sdk_tools/netcoredbg

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

%patch0 -p1

%build

%ifarch %{ix86}
export CFLAGS=" --target=i586-tizen-linux-gnu -Wno-deprecated-declarations"
export CXXFLAGS=" --target=i586-tizen-linux-gnu -Wno-deprecated-declarations"
export ASMFLAGS=" --target=i586-tizen-linux-gnu"
%else
export CFLAGS=" --target=%{_host} -Wno-deprecated-declarations"
export CXXFLAGS=" --target=%{_host} -Wno-deprecated-declarations"
export ASMFLAGS=" --target=%{_host}"
%endif

mkdir build
cd build
cmake ../netcoredbg \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCLR_BIN_DIR=%{_datarootdir}/%{netcoreappdir} \
    -DCLR_DIR=%{_datarootdir}/%{netcoreappdir} \
    -DCMAKE_INSTALL_PREFIX=%{install_prefix} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCLR_CMAKE_TARGET_ARCH_%{ARCH}=1 \
    -DCORECLR_SET_RPATH=%{_datarootdir}/%{netcoreappdir} \
    -DBUILD_MANAGED=OFF
make %{?jobs:-j%jobs}

%dotnet_build ../netcoredbg/src/debug/netcoredbg

%install
cd build
%make_install
mkdir -p %{buildroot}%{sdk_install_prefix}
mv %{buildroot}%{install_prefix}/netcoredbg %{buildroot}%{sdk_install_prefix}
install -p -m 644 ../netcoredbg/src/debug/netcoredbg/bin/*/*/SymbolReader.dll %{buildroot}%{sdk_install_prefix}

%files
%manifest netcoredbg.manifest
%{sdk_install_prefix}/*
