#pragma once
namespace forza2::lan {
void ValidateConfiguration();
void ConfigureIdentity();
void Shutdown();
bool SelfTestRequested();
bool RunTransportSelfTest();
} // namespace forza2::lan
