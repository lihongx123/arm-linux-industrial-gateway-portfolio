# 注册以后如何上传

准备好的发布目录包含README、LICENSE、源码、测试和证据；不包含本地Git历史。
推荐用GitHub Desktop上传整个发布目录，保留所有层级，不要只上传ZIP或几个截图。

1. 注册GitHub账号并验证邮箱，安装GitHub Desktop，在其中登录该账号。
2. 在Desktop选择 **File → New repository**。名称可用`mqmgateway-industrial-iot`。
   选择一个空的本地父目录；不生成README，不另选License，创建本地仓库。
3. 将本次发布目录**里面的全部文件和目录**复制到新仓库根目录（注意`.gitignore`等点文件）。
   根目录应直接看见`README.md`、`LICENSE`、`CMakeLists.txt`、`src/`。
4. 在Desktop的Changes逐项确认，提交信息可写
   `Add CAN/MQTT extension and reproducible ARM64 validation evidence`，点击Commit。
5. 点击 **Publish repository**。要作为简历公开链接，取消 **Keep this code private**。
   Description可填：`C++17 CAN/Modbus–MQTT gateway extension with Buildroot ARM64 validation and reproducible tests.`
6. 浏览器打开仓库，确认首页、LICENSE、贡献来源、测试表格和原始JSON链接可打开。
   在About中添加`cpp`、`mqtt`、`socketcan`、`modbus`、`buildroot`、`qemu`等主题。
7. 后续更新：先确认新证据对应的代码版本，再Commit，最后Push origin。

本次工作未创建GitHub账号、未登录、未创建远程仓库、未替用户发布任何文件。
不要向上游`BlackZork/mqmgateway`直接推送自己的项目。使用独立发布目录可避免沿用
工作目录原有的上游remote。可以在仓库首页保留来源链接，无须隐藏继承关系。

备用命令行方法（在发布目录的新副本中执行，替换用户名）：

```sh
git init -b main
git add .
git commit -m "Add industrial IoT extension and validation evidence"
git remote add origin https://github.com/YOUR_USERNAME/mqmgateway-industrial-iot.git
git push -u origin main
```

命令行方式需先在GitHub创建**空仓库**，不勾选自动生成README/License，以免初次推送冲突。
用Desktop登录认证即可；不要把令牌写进源码、远程URL或证据日志。

官方参考：[创建仓库](https://docs.github.com/en/get-started/start-your-journey/creating-a-repository-for-your-project-on-github)、
[文件大小限制](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github)。
网页单文件上传限25 MiB，普通Git拒绝超过100 MiB文件；本发布包排除系统镜像和构建产物。
