{
  description = "Cedit, a small terminal text editor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "cedit";
          version = nixpkgs.lib.fileContents ./VERSION;
          src = self;
          makeFlags = [ "PREFIX=$(out)" ];
          doCheck = true;
          checkTarget = "test";
          meta = {
            description = "Small terminal text editor with nano keys, Emacs keys and language-server support";
            homepage = "https://github.com/arcbaseproject/cedit";
            license = nixpkgs.lib.licenses.gpl3Plus;
            mainProgram = "cedit";
            platforms = systems;
          };
        };
      });
    };
}
