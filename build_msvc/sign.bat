@echo off
set CodeSignDescription=Bitcoin
set CodeSignDescriptionUrl=https://github.com/mateusz-klatt/bitcoin
set CodeSignFileDigest=sha384
set CodeSignTimestampUrl=http://timestamp.sectigo.com
set CodeSignTimestampDigest=sha384
set AzureKeyVaultUrl=https://codesignkeyvault.vault.azure.net/
set AzureKeyVaultClientId=38a04401-d8f1-4d37-ba2f-15b6d53f6cdf
set AzureKeyVaultClientSecret=%CODE_SIGNING_CLOUD%
set AzureKeyVaultCertificate=MateuszKlatt
set SignFile=%APPVEYOR_BUILD_FOLDER%\bitcoin-%APPVEYOR_BUILD_VERSION%-setup.exe

dotnet -d tool restore

dotnet tool run azuresigntool sign^
 --no-page-hashing --description "%CodeSignDescription%"^
 --description-url "%CodeSignDescriptionUrl%"^
 --file-digest "%CodeSignFileDigest%"^
 --timestamp-rfc3161 "%CodeSignTimestampUrl%"^
 --timestamp-digest "%CodeSignTimestampDigest%"^
 --azure-key-vault-url "%AzureKeyVaultUrl%"^
 --azure-key-vault-client-id "%AzureKeyVaultClientId%"^
 --azure-key-vault-client-secret "%AzureKeyVaultClientSecret%"^
 --azure-key-vault-certificate "%AzureKeyVaultCertificate%"^
 "%SignFile%"
