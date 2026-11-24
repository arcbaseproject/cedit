{
  lib,
  stdenv,
  fetchFromGitHub,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "cedit";
  version = "1.2.0";

  src = fetchFromGitHub {
    owner = "arcbaseproject";
    repo = "cedit";
    tag = "v${finalAttrs.version}";
    hash = "sha256-tnQQDBc2a6oYhmFCzN838tJ4MsbfIsz0+L0BcbSuy/o=";
  };

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  doCheck = true;
  checkTarget = "test";

  meta = {
    description = "Small terminal text editor with nano keys and language-server support";
    homepage = "https://github.com/arcbaseproject/cedit";
    changelog = "https://github.com/arcbaseproject/cedit/releases/tag/v${finalAttrs.version}";
    license = lib.licenses.gpl3Plus;
    maintainers = [ ];
    mainProgram = "cedit";
    platforms = lib.platforms.unix;
  };
})
