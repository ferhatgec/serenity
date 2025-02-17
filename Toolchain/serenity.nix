{ pkgs ? import <nixpkgs> { } }:
with pkgs;

stdenv.mkDerivation {
  name = "cpp-env";
  nativeBuildInputs = [
    ccache
    cmake
    curl
    e2fsprogs
    fuse2fs
    gcc12
    gmp
    # To create port launcher icons
    imagemagick
    libmpc
    mpfr
    ninja
    patch
    pkg-config
    qt6.qtbase
    qt6.qtsvg
    rsync
    texinfo
    unzip
  ];

  buildInputs = [
    e2fsprogs
    fuse2fs
    # To build the GRUB disk image
    grub2
    libxcrypt
    openssl
    parted
    qemu
    xlibsWrapper
  ];

  hardeningDisable = [ "format" "fortify" ];
}
