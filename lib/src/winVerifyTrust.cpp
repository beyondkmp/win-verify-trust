/*
Copyright (c) Anthony Beaumont
This source code is licensed under the MIT License
found in the LICENSE file in the root directory of this source tree.
*/

#include <napi.h>

#define _UNICODE 1
#define UNICODE 1

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust")
#pragma comment(lib, "Crypt32")

using namespace std;
#include <iostream>

std::wstring stringToWString(const std::string &s)
{
  int length;
  int slength = (int)s.length() + 1;
  length = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
  std::wstring buf;
  buf.resize(length);
  MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, const_cast<wchar_t *>(buf.c_str()), length);
  return buf;
}

static BOOL IsTrustedPublisherName(PCCERT_CHAIN_CONTEXT pChainContext, LPCWSTR publishName)
{
  PCERT_SIMPLE_CHAIN pChain;
  PCCERT_CONTEXT pCertContext;
  //
  // Get the publisher's certificate from the chain context
  //
  pChain = pChainContext->rgpChain[0];
  pCertContext = pChain->rgpElement[0]->pCertContext;

  LPWSTR AttrString = NULL;
  DWORD AttrStringLength;
  BOOL trusted = TRUE;

  //
  // First pass call to get the length of the subject name attribute.
  // Note, the returned length includes the NULL terminator.
  //

  AttrStringLength = CertGetNameStringW(
      pCertContext,
      CERT_NAME_ATTR_TYPE,
      0, // dwFlags
      (void *)szOID_ORGANIZATION_NAME,
      NULL, // AttrString
      0);
  // AttrStringLength

  if (AttrStringLength <= 1)
  {
    //
    // A matching certificate must have all of the attributes
    //

    return FALSE;
  }

  AttrString = (LPWSTR)LocalAlloc(
      LPTR,
      AttrStringLength * sizeof(WCHAR));
  if (AttrString == NULL)
  {
    return FALSE;
  }

  //
  // Second pass call to get the subject name attribute
  //

  AttrStringLength = CertGetNameStringW(
      pCertContext,
      CERT_NAME_ATTR_TYPE,
      0, // dwFlags
      (void *)szOID_ORGANIZATION_NAME,
      AttrString,
      AttrStringLength);

  if (AttrStringLength <= 1 || 0 != wcscmp(AttrString, publishName))
  {
    // The subject name attribute doesn't match
    trusted = FALSE;
  }

  LocalFree(AttrString);
  return trusted;
}

Napi::Object verifySignature(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  int length = info.Length();
  if (length != 2 || !info[0].IsString() || !info[1].IsString())
    Napi::TypeError::New(env, "String expected").ThrowAsJavaScriptException();
  Napi::String filePath = info[0].As<Napi::String>();
  Napi::String publishName = info[1].As<Napi::String>();
  Napi::Object result = Napi::Object::New(env);

  std::wstring wrapper = stringToWString(filePath);
  LPCWSTR pwszSourceFile = wrapper.c_str();

  std::wstring wrapper2 = stringToWString(publishName);
  LPCWSTR publishNameW = wrapper2.c_str();

  /*
  From https://docs.microsoft.com/en-us/windows/win32/seccrypto/example-c-program--verifying-the-signature-of-a-pe-file
  Copyright (C) Microsoft. All rights reserved.
  No copyright or trademark infringement is intended in using the aforementioned Microsoft example.
  */

  LONG lStatus;
  DWORD dwLastError;

  // Initialize the WINTRUST_FILE_INFO structure.

  WINTRUST_FILE_INFO FileData;
  memset(&FileData, 0, sizeof(FileData));
  FileData.cbStruct = sizeof(WINTRUST_FILE_INFO);
  FileData.pcwszFilePath = pwszSourceFile;
  FileData.hFile = NULL;
  FileData.pgKnownSubject = NULL;

  GUID WVTPolicyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA WinTrustData;

  // Initialize the WinVerifyTrust input data structure.

  // Default all fields to 0.
  memset(&WinTrustData, 0, sizeof(WinTrustData));

  WinTrustData.cbStruct = sizeof(WinTrustData);

  // Use default code signing EKU.
  WinTrustData.pPolicyCallbackData = NULL;

  // No data to pass to SIP.
  WinTrustData.pSIPClientData = NULL;

  // Disable WVT UI.
  WinTrustData.dwUIChoice = WTD_UI_NONE;

  // No revocation checking.
  WinTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;

  // Verify an embedded signature on a file.
  WinTrustData.dwUnionChoice = WTD_CHOICE_FILE;

  // Verify action.
  WinTrustData.dwStateAction = WTD_STATEACTION_VERIFY;

  // Verification sets this value.
  WinTrustData.hWVTStateData = NULL;

  // Not used.
  WinTrustData.pwszURLReference = NULL;

  // This is not applicable if there is no UI because it changes
  // the UI to accommodate running applications instead of
  // installing applications.
  WinTrustData.dwUIContext = 0;

  // Set pFile.
  WinTrustData.pFile = &FileData;

  // WinVerifyTrust verifies signatures as specified by the GUID
  // and Wintrust_Data.
  lStatus = WinVerifyTrust(NULL, &WVTPolicyGUID, &WinTrustData);

  switch (lStatus)
  {
  case ERROR_SUCCESS:
    result.Set("signed", true);
    result.Set("message", "The file is signed and the signature was verified");
    break;

  case TRUST_E_NOSIGNATURE:
    // The file was not signed or had a signature that was not valid.
    // Get the reason for no signature.
    dwLastError = GetLastError();
    if (TRUST_E_NOSIGNATURE == dwLastError ||
        TRUST_E_SUBJECT_FORM_UNKNOWN == dwLastError ||
        TRUST_E_PROVIDER_UNKNOWN == dwLastError)
    {
      result.Set("signed", false);
      result.Set("message", "The file is not signed");
    }
    else
    {
      result.Set("signed", false);
      result.Set("message", "An unknown error occurred trying to verify the signature of the file");
    }
    break;

  case TRUST_E_EXPLICIT_DISTRUST:
    result.Set("signed", false);
    result.Set("message", "The signature is present but specifically disallowed by the admin or user");
    break;

  case TRUST_E_SUBJECT_NOT_TRUSTED:
    result.Set("signed", false);
    result.Set("message", "The signature is present but not trusted");
    break;

  case CRYPT_E_SECURITY_SETTINGS:
    result.Set("signed", false);
    result.Set("message", "The signature wasn't explicitly trusted by the admin and admin policy has disabled user trust. No signature, publisher or timestamp errors");
    break;

  default:
    result.Set("signed", false);
    result.Set("message", "The UI was disabled in dwUIChoice or the admin policy has disabled user trust");
    break;
  }

  CRYPT_PROVIDER_DATA *pProvData = WTHelperProvDataFromStateData(WinTrustData.hWVTStateData);
  if (NULL == pProvData)
  {
    dwLastError = GetLastError();
    result.Set("signed", false);
    result.Set("message", "pProvData is null");
    goto Cleanup;
  }

  //
  // Get the signer
  //

  CRYPT_PROVIDER_SGNR *pProvSigner = WTHelperGetProvSignerFromChain(pProvData, 0, FALSE, 0);
  if (NULL == pProvSigner)
  {
    dwLastError = GetLastError();
    result.Set("signed", false);
    result.Set("message", "pProvSigner is null");
    goto Cleanup;
  }

  if (!IsTrustedPublisherName(pProvSigner->pChainContext, publishNameW))
  {
    dwLastError = GetLastError();
    result.Set("signed", false);
    result.Set("message", "IsTrustedPublisherName is false");
    goto Cleanup;
  }

Cleanup:
  // Any hWVTStateData must be released by a call with close.
  WinTrustData.dwStateAction = WTD_STATEACTION_CLOSE;

  lStatus = WinVerifyTrust(NULL, &WVTPolicyGUID, &WinTrustData);

  return result;
}

/* NAPI Initialize add-on*/

Napi::Object Init(Napi::Env env, Napi::Object exports)
{

  exports.Set("verifySignature", Napi::Function::New(env, verifySignature));
  return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)