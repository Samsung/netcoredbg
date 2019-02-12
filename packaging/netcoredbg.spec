Name:      netcoredbg
Summary:   Managed code debugger for CoreCLR
Version:   1.0.0
Release:   1
Group:     Development/Toolchain
License:   MIT
Source0:   netcoredbg.tar.gz
Source1001: netcoredbg.manifest
AutoReqProv: no


ExcludeArch: aarch64

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

# .NET Core Runtime
%define dotnetdir       dotnet
%define netshareddir    %{dotnetdir}/shared
%define netcoreapp      %{netshareddir}/Microsoft.NETCore.App/
%define netcoreappalias dotnet.tizen/netcoreapp
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
mkdir packaging/pkgs
ln -s /nuget packaging/pkgs/nuget

export CSVER=$(ls /nuget/microsoft.codeanalysis.common.*.nupkg | sort -n | tail -1 | cut -d "." -f4-6)

cp packaging/microsoft.codeanalysis.scripting.common.$CSVER.nupkg packaging/pkgs/
cp packaging/microsoft.codeanalysis.csharp.scripting.$CSVER.nupkg packaging/pkgs/

%build
export CFLAGS=" --target=%{_host}"
export CXXFLAGS=" --target=%{_host}"

%ifarch %{ix86}
export CFLAGS=$(echo $CFLAGS | sed -e 's/--target=i686/--target=i586/')
export CXXFLAGS=$(echo $CXXFLAGS | sed -e 's/--target=i686/--target=i586/')
%endif

export NETCOREAPPDIR=$(if [ -d %{_datarootdir}/%{netcoreappalias} ]; then echo %{_datarootdir}/%{netcoreappalias}; else find %{_datarootdir}/%{netcoreapp} -maxdepth 1 -type d -name '[0-9]*' -print | sort -n | tail -1; fi)

mkdir build
cd build
cmake ../netcoredbg \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCORECLR_DIR=$NETCOREAPPDIR \
    -DCMAKE_INSTALL_PREFIX=%{install_prefix} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCLR_CMAKE_LINUX_ID=tizen \
    -DDBGSHIM_RUNTIME_DIR=$NETCOREAPPDIR \
    -DBUILD_MANAGED=OFF
make %{?jobs:-j%jobs}

%dotnet_build -s ../netcoredbg/packaging/pkgs ../netcoredbg/src/debug/netcoredbg

%install
cd build
%make_install
mkdir -p %{buildroot}%{sdk_install_prefix}
mv %{buildroot}%{install_prefix}/netcoredbg %{buildroot}%{sdk_install_prefix}
install -p -m 644 ../netcoredbg/src/debug/netcoredbg/bin/*/*/SymbolReader.dll %{buildroot}%{sdk_install_prefix}

export CSVER=$(ls /nuget/microsoft.codeanalysis.common.*.nupkg | sort -n | tail -1 | cut -d "." -f4-6)
export SYSCODEPAGES=$(ls /nuget/system.text.encoding.codepages.4.*.nupkg | sort -n | tail -1)

unzip /nuget/microsoft.codeanalysis.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.dll
unzip /nuget/microsoft.codeanalysis.csharp.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.dll
unzip ../netcoredbg/packaging/microsoft.codeanalysis.scripting.common.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.Scripting.dll
unzip ../netcoredbg/packaging/microsoft.codeanalysis.csharp.scripting.$CSVER.nupkg lib/netstandard1.3/Microsoft.CodeAnalysis.CSharp.Scripting.dll
unzip $SYSCODEPAGES lib/netstandard1.3/System.Text.Encoding.CodePages.dll

find lib/netstandard1.3/ -name '*.dll' -exec chmod 644 {} \;
%ifnarch %{ix86}
find lib/netstandard1.3/ -name '*.dll' -exec %{_datarootdir}/%{netcoreappalias}/crossgen -ReadyToRun /Platform_Assemblies_Paths %{_datarootdir}/%{netcoreappalias}:$PWD/lib/netstandard1.3 {} \;
%endif

install -p -m 644 lib/netstandard1.3/*.dll %{buildroot}%{sdk_install_prefix}

%files
%manifest netcoredbg.manifest
%{sdk_install_prefix}/*
