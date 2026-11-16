#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh — сборка C# SDK AgentForge (AgentForge.cs + OrbRun.cs).
#
# Требуется .NET SDK 6.0+. Mono НЕ поддерживается: System.Text.Json не входит
# в mono/mcs; сборка под mono потребовала бы отдельного NuGet-пакета
# System.Text.Json и таргета netstandard2.0 — см. README_CSHARP.md.
#
# Скрипт при необходимости сам создаёт минимальный AgentForge.csproj
# (net6.0, OutputType Exe, точка входа Main в OrbRun.cs) и запускает
# `dotnet build`. Если dotnet не найден — печатает инструкцию установки
# и завершается с кодом 1.
# ---------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")"

CSPROJ="AgentForge.csproj"

# 1. Минимальный csproj, если его ещё нет.
if [ ! -f "$CSPROJ" ]; then
  cat > "$CSPROJ" <<'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net6.0</TargetFramework>
    <LangVersion>latest</LangVersion>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <OutputType>Exe</OutputType>
    <RootNamespace>AgentForgeSdk</RootNamespace>
    <AssemblyName>AgentForgeSdk</AssemblyName>
  </PropertyGroup>
</Project>
EOF
  echo "Создан $CSPROJ (net6.0, OutputType Exe, Main в OrbRun.cs)."
fi

# 2. Проверяем наличие .NET SDK.
if ! command -v dotnet >/dev/null 2>&1; then
  cat >&2 <<'EOTXT'
Ошибка: .NET SDK (команда dotnet) не найден — SDK AgentForge собирается
только им (System.Text.Json недоступен в mono/mcs).

Установи .NET SDK 6.0+ одним из способов:

  Ubuntu/Debian (репозиторий пакетов):
      sudo apt-get update && sudo apt-get install -y dotnet-sdk-6.0

  Универсальный способ (официальный скрипт dotnet-install):
      curl -fsSL https://dot.net/v1/dotnet-install.sh -o dotnet-install.sh
      bash dotnet-install.sh --channel 6.0
      export PATH="$HOME/.dotnet:$PATH"

Затем повтори сборку:  bash build.sh
EOTXT
  exit 1
fi

# 3. Сборка.
echo "dotnet: $(command -v dotnet)"
dotnet build "$CSPROJ" -c Release

echo
echo "Сборка завершена. Запуск примера OrbRun:"
echo "  dotnet run --project AgentForge.csproj -c Release -- afengine"
echo "('afengine' — путь к бинарнику движка; замени на свой, если движок не в PATH.)"
