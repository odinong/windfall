#pragma once
class Account
{
public:
    static const Account& GetAccount(uint32 persistentId) { static Account a; return a; }
    std::wstring_view GetMiiName() const { return L"Player"; }
};
