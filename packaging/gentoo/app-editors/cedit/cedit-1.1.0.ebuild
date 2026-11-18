# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit toolchain-funcs

DESCRIPTION="Small terminal text editor with nano keys and language-server support"
HOMEPAGE="https://github.com/arcbaseproject/cedit"
SRC_URI="https://github.com/arcbaseproject/cedit/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"

LICENSE="GPL-3+"
SLOT="0"
KEYWORDS="~amd64"
IUSE="lua"

DEPEND="lua? ( dev-lang/lua:5.4 )"
RDEPEND="${DEPEND}"
BDEPEND="lua? ( virtual/pkgconfig )"

src_compile() {
	emake CC="$(tc-getCC)" LUA=$(usex lua 1 0) LUA_PC=lua5.4
}

src_test() {
	emake CC="$(tc-getCC)" LUA=$(usex lua 1 0) LUA_PC=lua5.4 test
}

src_install() {
	emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" DOCDIR="${EPREFIX}/usr/share/doc/${PF}" install
}
