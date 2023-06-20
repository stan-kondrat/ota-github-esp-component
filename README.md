# Esp32 Over-The-Air(OTA) Github IDF Component


## Install 

1. Add **ota-github** dependency to **idf_component.yml**
```yml
dependencies:
  espressif/led_strip: "*"
  idf:
    version: ">=5"
  ota-github:
    path: .
    git: ssh://git@github.com/stan-kondrat/ota-github-esp-component.git
```

2. Reconfigure project
```sh
idf.py reconfigure
```

## Usage

1. To proceed with the first step, which is setting the app version, there are multiple options available:
- Add `set(PROJECT_VER "0.1.0")` to your `CMakeLists.txt` file.
- Use the `CONFIG_APP_PROJECT_VER_FROM_CONFIG option`.
- Store the version in a `version.txt` file.
- Assign the version based on a git tag using `git describe`.
For more details on how to configure the app version, please refer to the [documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/misc_system_api.html#app-version).

2. Create a GitHub release [manually](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository#creating-a-release), including a firmware binary artifact, using semver versioning. You can setup github workflow action, see example [here](https://github.com/stan-kondrat/ota-github-example).


3. Use `ota_github_install_latest()` function in your code:
```c
#include "ota_github.h";

// install latest release
ota_github_install_latest((ota_github_config_t){
    .github_user = "stan-kondrat",
    .github_repo = "ota-github-example",
    .firmawre_name = "firmawre.bin"
});
```

For a more advanced example that includes the ability to select specific versions and usage a GitHub Actions workflow, please visit the following link: https://github.com/stan-kondrat/ota-github-example .

## Similar projects

- https://github.com/Fishwaldo/esp_ghota (inspired by)
- https://github.com/scottchiefbaker/ESP-WebOTA


## License

MIT
