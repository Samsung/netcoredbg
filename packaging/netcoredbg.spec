Name:      netcoredbg
Summary:   Managed code debugger for CoreCLR
Version:   2.0.0
Release:   22
Group:     Development/Toolchain
License:   MIT
Source0:   %{name}-%{version}.tar.gz
Source1001: %{name}.manifest
AutoReqProv: no

# Accelerate clang
%ifarch armv7l
BuildRequires: clang-accel-armv7l-cross-arm
%endif
%ifarch aarch64
BuildRequires: clang-accel-aarch64-cross-aarch64
%endif

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
BuildRequires: unzip
BuildRequires: corefx-managed
BuildRequires: libdlog-devel
Requires: coreclr

%{!?build_type:%define build_type Release}
%{!?build_testing:%define build_testing OFF}

# .NET Core Runtime
%define dotnetdir       dotnet
%define netshareddir    %{dotnetdir}/shared
%define netcoreapp      %{netshareddir}/Microsoft.NETCore.App/
%define netcoreappalias dotnet.tizen/netcoreapp
%define sdktoolsdir     /home/owner/share/tmp/sdk_tools
%define install_prefix /usr
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

export NETCOREAPPDIR=$(if [ -d %{_datarootdir}/%{netcoreappalias} ]; then echo %{_datarootdir}/%{netcoreappalias}; else find %{_datarootdir}/%{netcoreapp} -maxdepth 1 -type d -name '[0-9]*' -print | sort -n | tail -1; fi)

mkdir build
cd build
cmake .. \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCORECLR_DIR=$NETCOREAPPDIR \
    -DCMAKE_INSTALL_PREFIX=%{install_prefix} \
    -DCMAKE_BUILD_TYPE=%{build_type} \
    -DCLR_CMAKE_LINUX_ID=tizen \
    -DDBGSHIM_RUNTIME_DIR=$NETCOREAPPDIR \
    -DBUILD_MANAGED=OFF \
    -DBUILD_TESTING=%{build_testing}

make %{?jobs:-j%jobs} %{?verbose:VERBOSE=1}

%dotnet_build -s ../packaging/pkgs ../src/managed

%install
cd build
%make_install
mkdir -p %{buildroot}%{sdk_install_prefix}
mv %{buildroot}%{install_prefix}/netcoredbg %{buildroot}%{sdk_install_prefix}
install -p -m 644 ../src/managed/bin/*/*/ManagedPart.dll %{buildroot}%{sdk_install_prefix}

export CSVER=$(ls /nuget/microsoft.codeanalysis.common.*.nupkg | sort -n | tail -1 | cut -d "." -f4-6)
export SYSCODEPAGES=$(ls /nuget/system.text.encoding.codepages.4.*.nupkg | sort -n | tail -1)

unzip /nuget/microsoft.codeanalysis.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.dll
unzip /nuget/microsoft.codeanalysis.csharp.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.dll
unzip ../packaging/microsoft.codeanalysis.scripting.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.Scripting.dll
unzip ../packaging/microsoft.codeanalysis.csharp.scripting.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.Scripting.dll
unzip $SYSCODEPAGES lib/netstandard1.3/System.Text.Encoding.CodePages.dll

find lib/netstandard1.3/ -name '*.dll' -exec chmod 644 {} \;
%ifnarch %{ix86}
find lib/netstandard1.3/ -name '*.dll' -exec %{_datarootdir}/%{netcoreappalias}/crossgen -ReadyToRun /Platform_Assemblies_Paths %{_datarootdir}/%{netcoreappalias}:$PWD/lib/netstandard1.3 {} \;
%endif

install -p -m 644 lib/netstandard1.3/*.dll %{buildroot}%{sdk_install_prefix}
touch %{buildroot}%{sdk_install_prefix}/version-%{version}

%files
%manifest netcoredbg.manifest
%{sdk_install_prefix}/*
