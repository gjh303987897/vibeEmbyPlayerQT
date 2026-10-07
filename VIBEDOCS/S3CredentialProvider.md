# S3 临时凭据与角色

模块提供 env/profile/instance 显式来源和 assumeRole，不自动探测 EC2。
使用 QNetworkAccessManager、Qt HMAC、已有 S3 SigV4，无额外 AWS SDK。

| 参数 | 行为 |
|---|---|
| --s3-secret-env NAME | 静态 secret，默认优先现有安全存储 |
| --s3-session-token-env NAME | 临时 SessionToken |
| --s3-credentials env | AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY/AWS_SESSION_TOKEN |
| --s3-credentials profile --s3-profile NAME | ~/.aws/credentials，支持 AWS_SHARED_CREDENTIALS_FILE/AWS_PROFILE |
| --s3-credentials instance | IMDSv2 token → 实例角色 → 临时凭据 |
| --s3-role-arn ARN | 用所选来源执行 STS AssumeRole |
| --s3-role-session-name NAME | 默认 vibePlayerQT-cli |
| --s3-external-id ID | 跨账户 ExternalId |

~~~powershell
.\vibePlayerQT tssl backup --target s3 --s3-endpoint https://s3.us-east-1.amazonaws.com --s3-bucket media --s3-region us-east-1 --s3-credentials env
.\vibePlayerQT tssl backup --target s3 --s3-endpoint https://s3.us-east-1.amazonaws.com --s3-bucket media --s3-region us-east-1 --s3-credentials profile --s3-profile default --s3-role-arn arn:aws:iam::123456789012:role/Backup
~~~

X-Amz-Security-Token 纳入签名，凭据换行被拒绝，不输出 secret/token。
IMDS 固定 link-local、无代理/重定向、支持取消、单步 2.5 秒上限，不退回 IMDSv1。
STS regional HTTPS（中国分区 amazonaws.com.cn）使用签名 POST，支持取消及
12 秒总时限；内部测试仅允许 loopback HTTP。检查 XML 完整性及凭据到期时间。
每次命令重新获取，不写 QSettings，长任务不在途中刷新令牌。
profile 读取 credentials，role_arn 通过显式参数；不自动执行 credential_process、
SSO 或 MFA。云权限和角色信任需实际账号验收；本地验证 profile、
STS 请求/响应/取消、S3 token header。
依据：[临时凭据](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_temp_use-resources.html)、
[IMDSv2](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/configuring-instance-metadata-service.html)、
[AssumeRole](https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRole.html)、
[SigV4](https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv-create-signed-request.html)。
