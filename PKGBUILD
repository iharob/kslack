# Maintainer: iharob <iharob@gmail.com>
pkgname=kslack
pkgver=1.0.0
pkgrel=1
pkgdesc="Slack desktop wrapper for KDE — Qt6 + KF6 with a titlebar that tracks Slack's chrome colour"
arch=('x86_64')
url="https://github.com/iharob/kslack"
license=('GPL3')
depends=(
  'qt6-base'
  'qt6-webengine'
  'qt6-svg'
  'kcoreaddons'
  'ki18n'
  'kxmlgui'
  'kcrash'
  'kdbusaddons'
  'knotifications'
  'kstatusnotifieritem'
  'kcolorscheme'
  'kconfigwidgets'
  'kconfig'
  'breeze-icons'
  'xdg-utils'
  'openssl'
  'sqlite'
)
makedepends=(
  'cmake'
  'extra-cmake-modules'
)
optdepends=(
  'kwallet: Chrome cookie import during sign-in'
)
source=("git+https://github.com/iharob/${pkgname}.git#tag=v${pkgver}")
sha256sums=('SKIP')

build() {
  cmake -B build -S "${pkgname}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF \
    -Wno-dev
  cmake --build build
}

package() {
  DESTDIR="${pkgdir}" cmake --install build
}
