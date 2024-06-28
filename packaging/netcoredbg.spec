Name:      netcoredbg
Summary:   Managed code debugger for CoreCLR
Version:   3.1.0
Release:   1
Group:     Development/Toolchain
License:   MIT
Source0:   %{name}-%{version}.tar.gz
Source1001: %{name}.manifest
AutoReqProv: no

%{!?coverage: %define coverage 0}
# Accelerate clang
%ifarch armv7l
BuildRequires: clang-accel-armv7l-cross-arm
%endif
%ifarch aarch64
BuildRequires: clang-accel-aarch64-cross-aarch64
%endif
%ifarch riscv64
BuildRequires: clang-accel-riscv64-cross-riscv64
%endif

BuildRequires: cmake
BuildRequires: clang >= 3.8
BuildRequires: clang-devel >= 3.8
BuildRequires: llvm >= 3.8
BuildRequires: llvm-devel >= 3.8
BuildRequires: libstdc++-devel
BuildRequires: coreclr-devel
BuildRequires: dotnet-build-tools
BuildRequires: unzip
BuildRequires: corefx-managed
BuildRequires: libdlog-devel
BuildRequires: libunwind-devel

%if 0%{coverage}
BuildRequires: compiler-rt
%endif

Requires: coreclr

%{!?build_type:%define build_type Release}
%{!?build_testing:%define build_testing OFF}

# .NET Core Runtime
%define dotnetdir       dotnet
%define netshareddir    %{dotnetdir}/shared
%define netcoreapp      %{netshareddir}/Microsoft.NETCore.App/
%define netcoreappalias dotnet.tizen/netcoreapp
%define sdktoolsdir     /home/owner/share/tmp/sdk_tools
%define netcoredbg_test %{sdktoolsdir}/netcoredbg-tests
%define install_prefix /usr
%define sdk_install_prefix /home/owner/share/tmp/sdk_tools/netcoredbg
%define netcoreapp_hotreload_min_ver 6.0.0

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

%ifarch riscv64
%define ARCH RISCV64
%endif

%description
This is a CoreCLR debugger for Tizen.

%package -n netcoredbg-test
Summary:  Native tests for netcoredbg
Requires: netcoredbg
AutoReqProv: no

%description -n netcoredbg-test
Tests native libs

%prep
%setup -q
cp %{SOURCE1001} .

mkdir packaging/pkgs
ln -s /nuget packaging/pkgs/nuget

export CSVER=$(ls /nuget/microsoft.codeanalysis.common.*.nupkg | sort -n | tail -1 | cut -d "." -f4-6)

cp packaging/microsoft.codeanalysis.scripting.common.$CSVER.nupkg packaging/pkgs/
cp packaging/microsoft.codeanalysis.csharp.scripting.$CSVER.nupkg packaging/pkgs/
cp packaging/nuget.xml tools/generrmsg/nuget.xml

%build
set -- %{vcs}
mkdir .git
printf "%s\n" "${1#*#}" > .git/HEAD
export CFLAGS=" --target=%{_host}"
export CXXFLAGS=" --target=%{_host}"

%ifarch %{ix86}
export CFLAGS=$(echo $CFLAGS | sed -e 's/--target=i686/--target=i586/')
export CXXFLAGS=$(echo $CXXFLAGS | sed -e 's/--target=i686/--target=i586/')
%endif

current_netcoreapp_version=$(for i in `find %{_datarootdir}/%{netcoreapp} -maxdepth 1 -type d -name '[0-9]*' -print`; do basename $i; done | sort -n | tail -1)
export NETCOREAPPDIR=$(if [ -d %{_datarootdir}/%{netcoreappalias} ]; then echo %{_datarootdir}/%{netcoreappalias}; else echo %{_datarootdir}/%{netcoreapp}/${current_netcoreapp_version}; fi)

echo "Building with .NET $current_netcoreapp_version"
is_hot_reload_supported=$(echo "$current_netcoreapp_version" %{netcoreapp_hotreload_min_ver} | awk '{print ($1 >= $2)}')

if [ "${is_hot_reload_supported}" == "1" ]; then
    STARTUP_HOOK=%{sdk_install_prefix}/ncdbhook.dll
fi

mkdir build
cd build
cmake .. \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCORECLR_DIR=$NETCOREAPPDIR \
    -DCMAKE_INSTALL_PREFIX=%{install_prefix} \
    -DCMAKE_BUILD_TYPE=%{build_type} \
    -DCLR_CMAKE_LINUX_ID=tizen \
    -DDBGSHIM_DIR=$NETCOREAPPDIR \
    -DBUILD_MANAGED=OFF \
%ifnarch riscv64
    -DNCDB_DOTNET_STARTUP_HOOK=$STARTUP_HOOK \
    -DINTEROP_DEBUGGING=1 \
%endif
    -DBUILD_TESTING=%{build_testing} \
    -DCLR_CMAKE_ENABLE_CODE_COVERAGE=%{coverage}

make %{?jobs:-j%jobs} %{?verbose:VERBOSE=1}

%define _dotnet_build_conf %{build_type}
%dotnet_build -s ../packaging/pkgs ../src/managed
# Build Debug only, code must be not optimized.
if [ "${is_hot_reload_supported}" == "1" ]; then
    NCDB_HOOK_TargetFramework=net6.0 dotnet build -c Debug ../src/ncdbhook
fi

# Native tests
# CLITestInteropBreakpoint
g++ -g -gdwarf-4 -fPIC -c "../test-suite/CLITestInteropBreakpoint/program.c" -o ./test_breakpoint.o
g++ -g -fPIC -shared -o libtest_breakpoint.so test_breakpoint.o

%install
cd build
%make_install
mkdir -p %{buildroot}%{sdk_install_prefix}
mv %{buildroot}%{install_prefix}/netcoredbg %{buildroot}%{sdk_install_prefix}
install -p -m 644 ../src/managed/bin/*/*/ManagedPart.dll %{buildroot}%{sdk_install_prefix}
if [ -f ../src/ncdbhook/bin/*/*/ncdbhook.dll ]; then
    install -p -m 644 ../src/ncdbhook/bin/*/*/ncdbhook.dll %{buildroot}%{sdk_install_prefix}
fi

export CSVER=$(ls /nuget/microsoft.codeanalysis.common.*.nupkg | sort -n | tail -1 | cut -d "." -f4-6)
export SYSCODEPAGES=$(ls /nuget/system.text.encoding.codepages.4.*.nupkg | sort -n | tail -1)

unzip /nuget/microsoft.codeanalysis.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.dll
unzip /nuget/microsoft.codeanalysis.csharp.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.dll
unzip ../packaging/microsoft.codeanalysis.scripting.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.Scripting.dll
unzip ../packaging/microsoft.codeanalysis.csharp.scripting.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.Scripting.dll
unzip $SYSCODEPAGES lib/netstandard1.3/System.Text.Encoding.CodePages.dll

find lib/netstandard1.3/ -name '*.dll' -exec chmod 644 {} \;
%ifnarch %{ix86} riscv64
find lib/netstandard1.3/ -name '*.dll' -exec %{_datarootdir}/%{netcoreappalias}/crossgen -ReadyToRun /Platform_Assemblies_Paths %{_datarootdir}/%{netcoreappalias}:$PWD/lib/netstandard1.3 {} \;
%endif

install -p -m 644 lib/netstandard1.3/*.dll %{buildroot}%{sdk_install_prefix}
touch %{buildroot}%{sdk_install_prefix}/version-%{version}-%{release}

mkdir -p %{buildroot}%{netcoredbg_test}
install -p -m 644 libtest_breakpoint.so %{buildroot}%{netcoredbg_test}

%files
%manifest netcoredbg.manifest
%{sdk_install_prefix}/*

%files -n netcoredbg-test
%manifest %{name}.manifest
%{netcoredbg_test}/*
