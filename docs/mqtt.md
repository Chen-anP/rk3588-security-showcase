# 9g MQTT 上报

在板子配置中加入以下段落，按实际部署填写 Broker 和设备 ID。出厂默认关闭 MQTT。

```ini
[mqtt]
enabled = true
host = broker.example.test
port = 8883
device_id = entrance-1
credentials = /etc/rk3588-security/mqtt_credentials
ca_file = /etc/rk3588-security/mqtt-ca.pem
```

凭据文件由程序运行用户持有，权限为 0600，格式为一行 `用户名:密码`。每台设备使用唯一的 device_id。Broker 启用 TLS 1.2 或更新版本并验证用户名密码，为设备账号配置仅发布其自身主题的 ACL。将签发 Broker 证书的 CA PEM 放在配置指向的路径。证书 SAN 必须匹配 host。正式凭据留在板子上。

`--ui` 和 `--access-run` 启动独立网络线程。红外状态取自现有门禁服务，未启用该服务时上报 unavailable。

| 主题 | 内容 | 保留 |
| --- | --- | --- |
| rksec/<device_id>/availability | online 布尔值，正常退出发离线，异常掉线由 Broker 发布遗嘱 | 是 |
| rksec/<device_id>/pir | state 取 active、idle、unavailable | 是 |
| rksec/<device_id>/passage | event_id、occurred_ms（Unix 毫秒）、result（simulated_grant） | 否 |

所有消息 QoS 1。通行记录写入成功且首次插入后入队。接收端按 event_id 去重。消息字段限定为表中列出的字段。通行事件总上限 128 条，含等待确认的通行事件。满载时淘汰最旧的待发事件并计数。红外状态合并为最新值。重连退避从 1 秒增加至 30 秒，重新发布当前状态并继续发送队列。队列在进程内存中，重启后从空队列开始，SQLite 保留正式通行历史。

验证：安装 libmosquitto-dev、mosquitto、mosquitto-clients、openssl 后运行 `python3 tests/mqtt_integration.py`。脚本生成临时凭据和证书，使用环回接口及合成数据，退出时清理临时进程和文件。
