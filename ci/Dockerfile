FROM fedora:31
MAINTAINER rpm-maint@lists.rpm.org

WORKDIR /srv/rpm

RUN echo -e "deltarpm=0\ninstall_weak_deps=0\ntsflags=nodocs" >> /etc/dnf/dnf.conf
RUN dnf -y update
RUN dnf -y install \
  autoconf \
  automake \
  libtool \
  gettext-devel \
  doxygen \
  make \
  gcc \
  git-core \
  zlib-devel \
  bzip2-devel \
  xz-devel \
  libzstd-devel \
  elfutils-libelf-devel \
  elfutils-devel \
  openssl-devel \
  file-devel \
  popt-devel \
  libarchive-devel \
  libdb-devel \
  lmdb-devel \
  libselinux-devel \
  ima-evm-utils-devel \
  libcap-devel \
  libacl-devel \
  audit-libs-devel \
  lua-devel readline-devel \
  dbus-devel \
  fakechroot which \
  elfutils binutils \
  findutils sed grep gawk diffutils file patch \
  tar unzip gzip bzip2 cpio xz \
  pkgconfig \
  /usr/bin/gdb-add-index \
  /usr/bin/python2 \
  python2-devel \
  dwz \
  && dnf clean all
COPY . .
RUN autoreconf -vfi
RUN ./configure \
  --with-crypto=openssl \
  --with-selinux \
  --with-cap \
  --with-acl \
  --with-lua \
<<<<<<< HEAD
  --enable-silent-rules \
=======
  --with-audit \
>>>>>>> ad16a04b6... Add plugin for logging audit events for package operations as per OSPP v4.2
  --enable-python \
  --enable-werror

RUN make -j

CMD make -j distcheck; rc=$?; find . -name rpmtests.log|xargs cat; exit $rc
