// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "HttpPrivatePCH.h"
#include "CurlHttpManager.h"
#include "CurlHttp.h"

#if WITH_LIBCURL

CURLM* FCurlHttpManager::GMultiHandle = NULL;
CURLSH* FCurlHttpManager::GShareHandle = NULL;

FCurlHttpManager::FCurlRequestOptions FCurlHttpManager::CurlRequestOptions;

void FCurlHttpManager::InitCurl()
{
	if (GMultiHandle != NULL)
	{
		UE_LOG(LogInit, Warning, TEXT("Already initialized multi handle"));
		return;
	}

	CURLcode InitResult = curl_global_init_mem(CURL_GLOBAL_ALL, CurlMalloc, CurlFree, CurlRealloc, CurlStrdup, CurlCalloc);
	if (InitResult == 0)
	{
		curl_version_info_data * VersionInfo = curl_version_info(CURLVERSION_NOW);
		if (VersionInfo)
		{
			UE_LOG(LogInit, Log, TEXT("Using libcurl %s"), ANSI_TO_TCHAR(VersionInfo->version));
			UE_LOG(LogInit, Log, TEXT(" - built for %s"), ANSI_TO_TCHAR(VersionInfo->host));

			if (VersionInfo->features & CURL_VERSION_SSL)
			{
				UE_LOG(LogInit, Log, TEXT(" - supports SSL with %s"), ANSI_TO_TCHAR(VersionInfo->ssl_version));
			}
			else
			{
				// No SSL
				UE_LOG(LogInit, Log, TEXT(" - NO SSL SUPPORT!"));
			}

			if (VersionInfo->features & CURL_VERSION_LIBZ)
			{
				UE_LOG(LogInit, Log, TEXT(" - supports HTTP deflate (compression) using libz %s"), ANSI_TO_TCHAR(VersionInfo->libz_version));
			}

			UE_LOG(LogInit, Log, TEXT(" - other features:"));

#define PrintCurlFeature(Feature)	\
			if (VersionInfo->features & Feature) \
			{ \
			UE_LOG(LogInit, Log, TEXT("     %s"), TEXT(#Feature));	\
			}

			PrintCurlFeature(CURL_VERSION_SSL);
			PrintCurlFeature(CURL_VERSION_LIBZ);

			PrintCurlFeature(CURL_VERSION_DEBUG);
			PrintCurlFeature(CURL_VERSION_IPV6);
			PrintCurlFeature(CURL_VERSION_ASYNCHDNS);
			PrintCurlFeature(CURL_VERSION_LARGEFILE);
			PrintCurlFeature(CURL_VERSION_IDN);
			PrintCurlFeature(CURL_VERSION_CONV);
			PrintCurlFeature(CURL_VERSION_TLSAUTH_SRP);
#undef PrintCurlFeature
		}

		GMultiHandle = curl_multi_init();
		if (NULL == GMultiHandle)
		{
			UE_LOG(LogInit, Fatal, TEXT("Could not initialize create libcurl multi handle! HTTP transfers will not function properly."));
		}

		GShareHandle = curl_share_init();
		if (NULL != GShareHandle)
		{
			curl_share_setopt(GShareHandle, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
			curl_share_setopt(GShareHandle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
			curl_share_setopt(GShareHandle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
		}
		else
		{
			UE_LOG(LogInit, Fatal, TEXT("Could not initialize libcurl share handle!"));
		}
	}
	else
	{
		UE_LOG(LogInit, Fatal, TEXT("Could not initialize libcurl (result=%d), HTTP transfers will not function properly."), (int32)InitResult);
	}

	// Init curl request options

	FString ProxyAddress;
	if (FParse::Value(FCommandLine::Get(), TEXT("httpproxy="), ProxyAddress))
	{
		if (!ProxyAddress.IsEmpty())
		{
			CurlRequestOptions.bUseHttpProxy = true;
			CurlRequestOptions.HttpProxyAddress = ProxyAddress;
		}
		else
		{
			UE_LOG(LogInit, Warning, TEXT(" Libcurl: -httpproxy has been passed as a parameter, but the address doesn't seem to be valid"));
		}
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("noreuseconn")))
	{
		CurlRequestOptions.bDontReuseConnections = true;
	}

	// discover cert location
	if (PLATFORM_LINUX)	// only relevant to Linux (for now?), not #ifdef'ed to keep the code checked by the compiler when compiling for other platforms
	{
		static const char * KnownBundlePaths[] =
		{
			"/etc/pki/tls/certs/ca-bundle.crt",
			"/etc/ssl/certs/ca-certificates.crt",
			"/etc/ssl/ca-bundle.pem",
			nullptr
		};

		for (const char ** CurrentBundle = KnownBundlePaths; *CurrentBundle; ++CurrentBundle)
		{
			FString FileName(*CurrentBundle);
			UE_LOG(LogInit, Log, TEXT(" Libcurl: checking if '%s' exists"), *FileName);

			if (FPaths::FileExists(FileName))
			{
				CurlRequestOptions.CertBundlePath = *CurrentBundle;
				break;
			}
		}
		if (CurlRequestOptions.CertBundlePath == nullptr)
		{
			UE_LOG(LogInit, Log, TEXT(" Libcurl: did not find a cert bundle in any of known locations, TLS may not work"));
		}
	}
#if PLATFORM_ANDROID
	// used #if here to protect against GExternalFilePath only available on Android
	else
	if (PLATFORM_ANDROID)
	{
		const int32 PathLength = 200;
		static ANSICHAR capath[PathLength] = { 0 };

		// if file does not already exist, create local PEM file with system trusted certificates
		extern FString GExternalFilePath;
		FString PEMFilename = GExternalFilePath / TEXT("ca-bundle.pem");
		if (!FPaths::FileExists(PEMFilename))
		{
			FString Contents;

			IFileManager* FileManager = &IFileManager::Get();
			auto Ar = TUniquePtr<FArchive>(FileManager->CreateFileWriter(*PEMFilename, 0));
			if (Ar)
			{
				// check for override ca-bundle.pem embedded in game content
				FString OverridePEMFilename = FPaths::GameContentDir() + TEXT("CurlCertificates/ca-bundle.pem");
				if (FFileHelper::LoadFileToString(Contents, *OverridePEMFilename))
				{
					const TCHAR* StrPtr = *Contents;
					auto Src = StringCast<ANSICHAR>(StrPtr, Contents.Len());
					Ar->Serialize((ANSICHAR*)Src.Get(), Src.Length() * sizeof(ANSICHAR));
				}
				else
				{
					// gather all the files in system certificates directory
					TArray<FString> directoriesToIgnoreAndNotRecurse;
					FLocalTimestampDirectoryVisitor Visitor(FPlatformFileManager::Get().GetPlatformFile(), directoriesToIgnoreAndNotRecurse, directoriesToIgnoreAndNotRecurse, false);
					FileManager->IterateDirectory(TEXT("/system/etc/security/cacerts"), Visitor);

					for (TMap<FString, FDateTime>::TIterator TimestampIt(Visitor.FileTimes); TimestampIt; ++TimestampIt)
					{
						// read and append the certificate file contents
						const FString CertFilename = TimestampIt.Key();
						if (FFileHelper::LoadFileToString(Contents, *CertFilename))
						{
							const TCHAR* StrPtr = *Contents;
							auto Src = StringCast<ANSICHAR>(StrPtr, Contents.Len());
							Ar->Serialize((ANSICHAR*)Src.Get(), Src.Length() * sizeof(ANSICHAR));
						}
					}

					// add optional additional certificates
					FString OptionalPEMFilename = FPaths::GameContentDir() + TEXT("CurlCertificates/ca-additions.pem");
					if (FFileHelper::LoadFileToString(Contents, *OptionalPEMFilename))
					{
						const TCHAR* StrPtr = *Contents;
						auto Src = StringCast<ANSICHAR>(StrPtr, Contents.Len());
						Ar->Serialize((ANSICHAR*)Src.Get(), Src.Length() * sizeof(ANSICHAR));
					}
				}

				FPlatformString::Strncpy(capath, TCHAR_TO_ANSI(*PEMFilename), PathLength);
				CurlRequestOptions.CertBundlePath = capath;
				UE_LOG(LogInit, Log, TEXT(" Libcurl: using generated PEM file: '%s'"), *PEMFilename);
			}
		}
		else
		{
			FPlatformString::Strncpy(capath, TCHAR_TO_ANSI(*PEMFilename), PathLength);
			CurlRequestOptions.CertBundlePath = capath;
			UE_LOG(LogInit, Log, TEXT(" Libcurl: using existing PEM file: '%s'"), *PEMFilename);
		}

		if (CurlRequestOptions.CertBundlePath == nullptr)
		{
			UE_LOG(LogInit, Log, TEXT(" Libcurl: failed to generate a PEM cert bundle, TLS may not work"));
		}
	}
#endif

	// set certificate verification (disable to allow self-signed certificates)
	if (CurlRequestOptions.CertBundlePath == nullptr)
	{
		CurlRequestOptions.bVerifyPeer = false;
	}
	else
	{
		bool bVerifyPeer = true;
		if (GConfig->GetBool(TEXT("/Script/Engine.NetworkSettings"), TEXT("n.VerifyPeer"), bVerifyPeer, GEngineIni))
		{
			CurlRequestOptions.bVerifyPeer = bVerifyPeer;
		}
	}

	// print for visibility
	CurlRequestOptions.Log();
}

void FCurlHttpManager::FCurlRequestOptions::Log()
{
	UE_LOG(LogInit, Log, TEXT(" CurlRequestOptions (configurable via config and command line):"));
		UE_LOG(LogInit, Log, TEXT(" - bVerifyPeer = %s  - Libcurl will %sverify peer certificate"),
		bVerifyPeer ? TEXT("true") : TEXT("false"),
		bVerifyPeer ? TEXT("") : TEXT("NOT ")
		);

	UE_LOG(LogInit, Log, TEXT(" - bUseHttpProxy = %s  - Libcurl will %suse HTTP proxy"),
		bUseHttpProxy ? TEXT("true") : TEXT("false"),
		bUseHttpProxy ? TEXT("") : TEXT("NOT ")
		);	
	if (bUseHttpProxy)
	{
		UE_LOG(LogInit, Log, TEXT(" - HttpProxyAddress = '%s'"), *CurlRequestOptions.HttpProxyAddress);
	}

	UE_LOG(LogInit, Log, TEXT(" - bDontReuseConnections = %s  - Libcurl will %sreuse connections"),
		bDontReuseConnections ? TEXT("true") : TEXT("false"),
		bDontReuseConnections ? TEXT("NOT ") : TEXT("")
		);

	UE_LOG(LogInit, Log, TEXT(" - CertBundlePath = %s  - Libcurl will %s"),
		(CertBundlePath != nullptr) ? *FString(CertBundlePath) : TEXT("nullptr"),
		(CertBundlePath != nullptr) ? TEXT("set CURLOPT_CAINFO to it") : TEXT("use whatever was configured at build time.")
		);
}


void FCurlHttpManager::ShutdownCurl()
{
	if (NULL != GMultiHandle)
	{
		curl_multi_cleanup(GMultiHandle);
		GMultiHandle = NULL;
	}

	curl_global_cleanup();
}

FCurlHttpManager::FCurlHttpManager()
	:	FHttpManager()
	,	MultiHandle(GMultiHandle)
	,	MaxSimultaneousRequests(0)
	,	MaxRequestsAddedPerFrame(0)
	,	NumRequestsAddedToMulti(0)
{
	check(MultiHandle);
	if (GConfig)
	{
		GConfig->GetInt(TEXT("HTTP"), TEXT("CurlMaxSimultaneousRequests"), MaxSimultaneousRequests, GEngineIni);
		GConfig->GetInt(TEXT("HTTP"), TEXT("CurlMaxRequestsAddedPerFrame"), MaxRequestsAddedPerFrame, GEngineIni);
	}
}

void FCurlHttpManager::AddRequest(const TSharedRef<IHttpRequest>& Request)
{
	checkf(false, TEXT("Should not be called for curl http anymore, should be using AddThreadedRequest."));
}

void FCurlHttpManager::RemoveRequest(const TSharedRef<IHttpRequest>& Request)
{
	checkf(false, TEXT("Should not be called for curl http anymore."));
}

bool FCurlHttpManager::FindNextEasyHandle(CURL** OutEasyHandle) const
{
	FDateTime LowestDateTime = FDateTime::MaxValue();
	for (auto& It : HandlesToRequests)
	{
		if (!It.Value.bProcessingStarted)
		{
			if (It.Value.DateTime < LowestDateTime)
			{
				LowestDateTime = It.Value.DateTime;
				*OutEasyHandle = It.Key;
			}
		}
	}
	return LowestDateTime != FDateTime::MaxValue();
}

void FCurlHttpManager::HttpThreadTick(float DeltaSeconds)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FCurlHttpManager_Tick);
	check(MultiHandle);

	double CurlTick = 0.0;
	const int NumRequestsToTick = Requests.Num();

	{
		FSimpleScopeSecondsCounter CurlTickTimer(CurlTick);

		if (RunningThreadedRequests.Num() > 0)
		{
			int RunningRequests = -1;
			curl_multi_perform(MultiHandle, &RunningRequests);

			// read more info if number of requests changed or if there's zero running
			// (note that some requests might have never be "running" from libcurl's point of view)
			if (RunningRequests == 0 || RunningRequests != NumRequestsAddedToMulti)
			{
				for(;;)
				{
					int MsgsStillInQueue = 0;	// may use that to impose some upper limit we may spend in that loop
					CURLMsg * Message = curl_multi_info_read(MultiHandle, &MsgsStillInQueue);

					if (Message == NULL)
					{
						break;
					}

					// find out which requests have completed
					if (Message->msg == CURLMSG_DONE)
					{
						CURL* CompletedHandle = Message->easy_handle;
						curl_multi_remove_handle(GMultiHandle, CompletedHandle);
						--NumRequestsAddedToMulti;

						CurlEasyRequestData* RequestData = HandlesToRequests.Find(CompletedHandle);
						if (RequestData)
						{
							RequestData->bAddedToMulti = false;

							FCurlHttpRequest* CurlRequest = static_cast< FCurlHttpRequest* >(RequestData->Request);
							CurlRequest->MarkAsCompleted(Message->data.result);

							UE_LOG(LogHttp, Verbose, TEXT("Request %p (easy handle:%p) has completed (code:%d) and has been marked as such"), CurlRequest, CompletedHandle, (int32)Message->data.result);
						}
						else
						{
							UE_LOG(LogHttp, Warning, TEXT("Could not find mapping for completed request (easy handle: %p)"), CompletedHandle);
						}
					}
				}
			}

			int NumAdded = 0;
			CURL* EasyHandle;
			while ((MaxSimultaneousRequests == 0 || NumRequestsAddedToMulti < MaxSimultaneousRequests) &&
				(MaxRequestsAddedPerFrame == 0 || NumAdded < MaxRequestsAddedPerFrame) &&
				FindNextEasyHandle(&EasyHandle))
			{
				CurlEasyRequestData& RequestData = HandlesToRequests.FindChecked(EasyHandle);

				CURLMcode AddResult = curl_multi_add_handle(GMultiHandle, EasyHandle);
				RequestData.bProcessingStarted = true;

				if (AddResult == CURLM_OK)
				{
					++NumAdded;
					++NumRequestsAddedToMulti;
					RequestData.bAddedToMulti = true;
				}
				else
				{
					UE_LOG(LogHttp, Warning, TEXT("Failed to add easy handle %p to multi handle with code %d"), EasyHandle, (int)AddResult);
				}

				FCurlHttpRequest* CurlRequest = static_cast<FCurlHttpRequest*>(RequestData.Request);
				CurlRequest->SetAddToCurlMultiResult(AddResult);
			}
		}
	}

	double ParentTick = 0.0;
	{
		FSimpleScopeSecondsCounter TickTimer(ParentTick);
		FHttpManager::HttpThreadTick(DeltaSeconds);
	}

	if (ParentTick + CurlTick > 0.02)
	{
		UE_LOG(LogHttp, Warning, TEXT("HITCHHUNTER: Hitch in CurlHttp (CurlTick: %.1f ms, HttpManagerTick: %.1f) has been detected this frame, NumRequestsToTick = %d"), CurlTick * 1000.0, ParentTick * 1000.0, NumRequestsToTick);
	}
}

bool FCurlHttpManager::StartThreadedRequest(IHttpThreadedRequest* Request)
{
	FCurlHttpRequest* CurlRequest = static_cast<FCurlHttpRequest*>(Request);
	CURL* EasyHandle = CurlRequest->GetEasyHandle();
	ensure(!HandlesToRequests.Contains(EasyHandle));
	HandlesToRequests.Add(EasyHandle, CurlEasyRequestData(Request));

	return true;
}

void FCurlHttpManager::CompleteThreadedRequest(IHttpThreadedRequest* Request)
{
	FCurlHttpRequest* CurlRequest = static_cast<FCurlHttpRequest*>(Request);
	CURL* EasyHandle = CurlRequest->GetEasyHandle();

	CurlEasyRequestData* RequestData = HandlesToRequests.Find(EasyHandle);
	ensure(RequestData);
	if (RequestData)
	{
		if (RequestData->bAddedToMulti)
		{
			curl_multi_remove_handle(GMultiHandle, EasyHandle);
			--NumRequestsAddedToMulti;
		}
		HandlesToRequests.Remove(EasyHandle);
	}
}

#endif //WITH_LIBCURL
