#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <portabledeviceapi.h>
#include <portabledevice.h>
#include <portabledevicetypes.h>
#include <WpdMtpExtensions.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string utf8(const wchar_t *text) {
    if (!text) return {};
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                       nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                           result.data(), required, nullptr, nullptr) != required) return {};
    result.pop_back();
    return result;
}

void json_string(const wchar_t *text) {
    const std::string bytes = utf8(text);
    std::putchar('"');
    for (unsigned char c : bytes) {
        if (c == '"' || c == '\\') { std::putchar('\\'); std::putchar(c); }
        else if (c < 0x20) std::printf("\\u%04x", c);
        else std::putchar(c);
    }
    std::putchar('"');
}

int fail(const char *step, HRESULT error) {
    std::fprintf(stderr, "%s: 0x%08lx\n", step, static_cast<unsigned long>(error));
    return 1;
}

struct Device {
    ComPtr<IPortableDevice> device;
    ComPtr<IPortableDeviceContent> content;
};

HRESULT open_vcm(Device &out, bool writable = false) {
    ComPtr<IPortableDeviceManager> manager;
    HRESULT hr = CoCreateInstance(CLSID_PortableDeviceManager, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr)) return hr;
    DWORD count = 0;
    hr = manager->GetDevices(nullptr, &count);
    if (FAILED(hr)) return hr;
    if (!count) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    std::vector<PWSTR> ids(count, nullptr);
    hr = manager->GetDevices(ids.data(), &count);
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < count && !out.device; ++i) {
        if (!ids[i]) continue;
        std::wstring upper_id(ids[i]);
        for (wchar_t &c : upper_id) c = static_cast<wchar_t>(towupper(c));
        if (upper_id.find(L"VID_054C&PID_04E4") == std::wstring::npos) continue;
        DWORD length = 0;
        hr = manager->GetDeviceFriendlyName(ids[i], nullptr, &length);
        if (FAILED(hr) || !length || length > 256) continue;
        std::vector<wchar_t> name(length);
        hr = manager->GetDeviceFriendlyName(ids[i], name.data(), &length);
        if (FAILED(hr) || wcscmp(name.data(), L"Virtual Media Library")) continue;
        ComPtr<IPortableDevice> candidate;
        hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&candidate));
        if (FAILED(hr)) break;
        ComPtr<IPortableDeviceValues> client;
        hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client));
        if (FAILED(hr)) break;
        client->SetStringValue(WPD_CLIENT_NAME, L"Content Manager Pro");
        client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS,
                                        writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
        hr = candidate->Open(ids[i], client.Get());
        if (SUCCEEDED(hr)) out.device = candidate;
    }
    for (PWSTR id : ids) CoTaskMemFree(id);
    if (!out.device) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    return out.device->Content(&out.content);
}

struct Item {
    std::wstring id, parent, name, title, artist, album, created, modified, format, content_type;
    ULONGLONG size = 0;
    ULONG width = 0, height = 0;
    bool folder = false, hidden = false, system = false;
};

std::wstring guid_property(IPortableDeviceValues *values, const PROPERTYKEY &key) {
    GUID value{};
    wchar_t text[40]{};
    if (SUCCEEDED(values->GetGuidValue(key, &value))) StringFromGUID2(value, text, 40);
    return text;
}

std::wstring string_property(IPortableDeviceValues *values, const PROPERTYKEY &key) {
    PWSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(values->GetStringValue(key, &value)) && value) result = value;
    CoTaskMemFree(value);
    return result;
}

std::wstring date_property(IPortableDeviceValues *values, const PROPERTYKEY &key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(values->GetValue(key, &value)) && value.vt == VT_DATE) {
        SYSTEMTIME time{};
        if (VariantTimeToSystemTime(value.date, &time)) {
            wchar_t stamp[32];
            swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u", time.wYear,
                       time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
            result = stamp;
        }
    }
    PropVariantClear(&value);
    return result;
}

HRESULT read_item(IPortableDeviceProperties *properties, const wchar_t *id, Item &item) {
    ComPtr<IPortableDeviceValues> values;
    HRESULT hr = properties->GetValues(id, nullptr, &values);
    if (FAILED(hr)) return hr;
    item.id = id;
    PWSTR value = nullptr;
    if (SUCCEEDED(values->GetStringValue(WPD_OBJECT_PARENT_ID, &value))) {
        item.parent = value;
        CoTaskMemFree(value);
    }
    value = nullptr;
    if (SUCCEEDED(values->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &value)) && value[0]) {
        item.name = value;
        CoTaskMemFree(value);
    } else {
        CoTaskMemFree(value);
        value = nullptr;
        if (SUCCEEDED(values->GetStringValue(WPD_OBJECT_NAME, &value))) {
            item.name = value;
            CoTaskMemFree(value);
        }
    }
    values->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &item.size);
    item.title = string_property(values.Get(), WPD_MEDIA_TITLE);
    if (item.title.empty()) item.title = string_property(values.Get(), WPD_OBJECT_NAME);
    item.artist = string_property(values.Get(), WPD_MEDIA_ARTIST);
    item.album = string_property(values.Get(), WPD_MUSIC_ALBUM);
    item.created = date_property(values.Get(), WPD_OBJECT_DATE_CREATED);
    item.modified = date_property(values.Get(), WPD_OBJECT_DATE_MODIFIED);
    values->GetUnsignedIntegerValue(WPD_MEDIA_WIDTH, &item.width);
    values->GetUnsignedIntegerValue(WPD_MEDIA_HEIGHT, &item.height);
    item.format = guid_property(values.Get(), WPD_OBJECT_FORMAT);
    item.content_type = guid_property(values.Get(), WPD_OBJECT_CONTENT_TYPE);
    BOOL flag = FALSE;
    if (SUCCEEDED(values->GetBoolValue(WPD_OBJECT_ISHIDDEN, &flag))) item.hidden = flag != FALSE;
    flag = FALSE;
    if (SUCCEEDED(values->GetBoolValue(WPD_OBJECT_ISSYSTEM, &flag))) item.system = flag != FALSE;
    GUID kind{};
    if (SUCCEEDED(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &kind)))
        item.folder = IsEqualGUID(kind, WPD_CONTENT_TYPE_FOLDER) ||
                      IsEqualGUID(kind, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
    return S_OK;
}

HRESULT enumerate(IPortableDeviceContent *content, IPortableDeviceProperties *properties,
                  const wchar_t *parent, unsigned depth, std::vector<Item> &items,
                  std::unordered_set<std::wstring> &seen) {
    if (depth > 4 || items.size() >= 100000) return E_UNEXPECTED;
    ComPtr<IEnumPortableDeviceObjectIDs> objects;
    HRESULT hr = content->EnumObjects(0, parent, nullptr, &objects);
    if (FAILED(hr)) return hr;
    unsigned scanned = 0;
    while (scanned++ < 100000) {
        PWSTR id = nullptr;
        ULONG fetched = 0;
        hr = objects->Next(1, &id, &fetched);
        if (hr == S_FALSE || !fetched) return S_OK;
        if (FAILED(hr)) return hr;
        Item item;
        hr = read_item(properties, id, item);
        CoTaskMemFree(id);
        if (FAILED(hr)) return hr;
        if (!seen.insert(item.id).second) continue;
        items.push_back(item);
        if (item.folder) {
            hr = enumerate(content, properties, item.id.c_str(), depth + 1, items, seen);
            if (FAILED(hr)) return hr;
        }
    }
    return E_UNEXPECTED;
}

int list(Device &device) {
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = device.content->Properties(&properties);
    if (FAILED(hr)) return fail("Properties", hr);
    std::vector<Item> items;
    std::unordered_set<std::wstring> seen;
    hr = enumerate(device.content.Get(), properties.Get(), WPD_DEVICE_OBJECT_ID, 0, items, seen);
    if (FAILED(hr)) return fail("EnumObjects", hr);
    std::fputs("{\"items\":[", stdout);
    for (size_t i = 0; i < items.size(); ++i) {
        const Item &item = items[i];
        if (i) std::putchar(',');
        std::fputs("{\"id\":", stdout); json_string(item.id.c_str());
        std::fputs(",\"parent\":", stdout); json_string(item.parent.c_str());
        std::fputs(",\"name\":", stdout); json_string(item.name.c_str());
        std::fputs(",\"title\":", stdout); json_string(item.title.c_str());
        std::fputs(",\"artist\":", stdout); json_string(item.artist.c_str());
        std::fputs(",\"album\":", stdout); json_string(item.album.c_str());
        std::fputs(",\"created\":", stdout); json_string(item.created.c_str());
        std::fputs(",\"modified\":", stdout); json_string(item.modified.c_str());
        std::fputs(",\"format\":", stdout); json_string(item.format.c_str());
        std::fputs(",\"content_type\":", stdout); json_string(item.content_type.c_str());
        std::printf(",\"hidden\":%s,\"system\":%s", item.hidden ? "true" : "false", item.system ? "true" : "false");
        std::printf(",\"size\":%llu,\"width\":%lu,\"height\":%lu,\"folder\":%s}",
                    item.size, item.width, item.height,
                    item.folder ? "true" : "false");
    }
    std::fputs("]}\n", stdout);
    return 0;
}

int attributes(Device &device, const wchar_t *object_id) {
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = device.content->Properties(&properties);
    if (FAILED(hr)) return fail("Properties", hr);
    struct Candidate { const char *name; const PROPERTYKEY *key; };
    const Candidate keys[] = {
        {"name", &WPD_OBJECT_NAME}, {"file", &WPD_OBJECT_ORIGINAL_FILE_NAME},
        {"size", &WPD_OBJECT_SIZE}, {"created", &WPD_OBJECT_DATE_CREATED},
        {"modified", &WPD_OBJECT_DATE_MODIFIED}, {"title", &WPD_MEDIA_TITLE},
        {"artist", &WPD_MEDIA_ARTIST}, {"album", &WPD_MUSIC_ALBUM},
        {"format", &WPD_OBJECT_FORMAT}, {"type", &WPD_OBJECT_CONTENT_TYPE},
        {"persistent", &WPD_OBJECT_PERSISTENT_UNIQUE_ID},
        {"hidden", &WPD_OBJECT_ISHIDDEN}, {"system", &WPD_OBJECT_ISSYSTEM}
    };
    for (const Candidate &key : keys) {
        ComPtr<IPortableDeviceValues> values;
        hr = properties->GetPropertyAttributes(object_id, *key.key, &values);
        std::printf("%s: 0x%08lx\n", key.name, static_cast<unsigned long>(hr));
    }
    return 0;
}

int vendor_probe(Device &device) {
    ComPtr<IPortableDeviceValues> parameters, result;
    ComPtr<IPortableDevicePropVariantCollection> operation_parameters;
    HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&parameters));
    if (FAILED(hr)) return fail("Vendor params", hr);
    hr = CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation_parameters));
    if (FAILED(hr)) return fail("Vendor operation params", hr);
    hr = parameters->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,
                                  WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.fmtid);
    if (SUCCEEDED(hr)) hr = parameters->SetUnsignedIntegerValue(
        WPD_PROPERTY_COMMON_COMMAND_ID,
        WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE.pid);
    if (SUCCEEDED(hr)) hr = parameters->SetUnsignedIntegerValue(
        WPD_PROPERTY_MTP_EXT_OPERATION_CODE, 0x9ff0);
    if (SUCCEEDED(hr)) hr = parameters->SetIPortableDevicePropVariantCollectionValue(
        WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, operation_parameters.Get());
    if (FAILED(hr)) return fail("Vendor command parameters", hr);
    hr = device.device->SendCommand(0, parameters.Get(), &result);
    if (FAILED(hr)) return fail("Vendor probe", hr);
    ULONG response = 0;
    hr = result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, &response);
    if (FAILED(hr)) return fail("Vendor probe response", hr);
    std::printf("response=0x%04lx\n", static_cast<unsigned long>(response));
    return 0;
}

ComPtr<IPortableDevicePropVariantCollection> uint_params(const std::vector<ULONG> &values) {
    ComPtr<IPortableDevicePropVariantCollection> result;
    if (FAILED(CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result)))) return {};
    for (ULONG value : values) {
        PROPVARIANT item; PropVariantInit(&item); item.vt=VT_UI4; item.ulVal=value;
        if (FAILED(result->Add(&item))) return {};
    }
    return result;
}

HRESULT vendor_parameters(IPortableDeviceValues *parameters, const PROPERTYKEY &command,
                          ULONG operation, const std::vector<ULONG> &values) {
    HRESULT hr=parameters->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,command.fmtid);
    if(SUCCEEDED(hr)) hr=parameters->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,command.pid);
    if(SUCCEEDED(hr)) hr=parameters->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,operation);
    auto collection=uint_params(values);
    if(!collection) return E_OUTOFMEMORY;
    if(SUCCEEDED(hr)) hr=parameters->SetIPortableDevicePropVariantCollectionValue(
        WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,collection.Get());
    return hr;
}

int vendor_no_data(Device &device, ULONG operation, const std::vector<ULONG> &values,
                   std::vector<ULONG> *response_values=nullptr) {
    ComPtr<IPortableDeviceValues> parameters,result;
    HRESULT hr=CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&parameters));
    if(SUCCEEDED(hr)) hr=vendor_parameters(parameters.Get(),
        WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE,operation,values);
    if(SUCCEEDED(hr)) hr=device.device->SendCommand(0,parameters.Get(),&result);
    if(FAILED(hr)) return fail("Vendor command",hr);
    ULONG response=0; hr=result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,&response);
    if(FAILED(hr)) return fail("Vendor response",hr);
    if(response!=0x2001) { std::fprintf(stderr,"Vita response: 0x%04lx\n",response); return 1; }
    if(response_values) {
        ComPtr<IPortableDevicePropVariantCollection> collection;
        if(SUCCEEDED(result->GetIPortableDevicePropVariantCollectionValue(
            WPD_PROPERTY_MTP_EXT_RESPONSE_PARAMS,&collection))) {
            DWORD count=0; collection->GetCount(&count);
            for(DWORD i=0;i<count;++i) { PROPVARIANT item; PropVariantInit(&item);
                if(SUCCEEDED(collection->GetAt(i,&item)) && item.vt==VT_UI4)
                    response_values->push_back(item.ulVal);
                PropVariantClear(&item);
            }
        }
    }
    return 0;
}

int vendor_write(Device &device, ULONG operation, const std::vector<ULONG> &values,
                 const wchar_t *path) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return fail("Open upload",HRESULT_FROM_WIN32(GetLastError()));
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(file,&length) || length.QuadPart<=0) { CloseHandle(file); return fail("Upload size",E_INVALIDARG); }
    ComPtr<IPortableDeviceValues> parameters,result;
    HRESULT hr=CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&parameters));
    if(SUCCEEDED(hr)) hr=vendor_parameters(parameters.Get(),
        WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE,operation,values);
    if(SUCCEEDED(hr)) hr=parameters->SetUnsignedLargeIntegerValue(
        WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE,(ULONGLONG)length.QuadPart);
    if(SUCCEEDED(hr)) hr=device.device->SendCommand(0,parameters.Get(),&result);
    PWSTR context=nullptr; ULONG optimal=0;
    if(SUCCEEDED(hr)) hr=result->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT,&context);
    if(SUCCEEDED(hr)) hr=result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE,&optimal);
    if(FAILED(hr) || !context) { CloseHandle(file); CoTaskMemFree(context); return fail("Begin upload",hr); }
    if(optimal<4096 || optimal>1024*1024) optimal=64*1024;
    std::vector<BYTE> buffer(optimal); ULONGLONG sent=0;
    while(SUCCEEDED(hr) && sent<(ULONGLONG)length.QuadPart) {
        DWORD got=0; if(!ReadFile(file,buffer.data(),optimal,&got,nullptr) || !got) { hr=E_FAIL; break; }
        parameters->Clear();
        hr=parameters->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,WPD_COMMAND_MTP_EXT_WRITE_DATA.fmtid);
        if(SUCCEEDED(hr)) hr=parameters->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,WPD_COMMAND_MTP_EXT_WRITE_DATA.pid);
        if(SUCCEEDED(hr)) hr=parameters->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT,context);
        if(SUCCEEDED(hr)) hr=parameters->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE,got);
        if(SUCCEEDED(hr)) hr=parameters->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA,buffer.data(),got);
        result.Reset(); if(SUCCEEDED(hr)) hr=device.device->SendCommand(0,parameters.Get(),&result);
        ULONG written=0; if(SUCCEEDED(hr)) hr=result->GetUnsignedIntegerValue(
            WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN,&written);
        if(SUCCEEDED(hr) && written!=got) hr=E_FAIL;
        sent+=written;
        if(SUCCEEDED(hr)) { std::printf("{\"sent\":%llu,\"total\":%lld}\n",sent,length.QuadPart); std::fflush(stdout); }
    }
    CloseHandle(file);
    parameters->Clear();
    if(SUCCEEDED(hr)) hr=parameters->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.fmtid);
    if(SUCCEEDED(hr)) hr=parameters->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid);
    if(SUCCEEDED(hr)) hr=parameters->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT,context);
    result.Reset(); if(SUCCEEDED(hr)) hr=device.device->SendCommand(0,parameters.Get(),&result);
    CoTaskMemFree(context);
    ULONG response=0; if(SUCCEEDED(hr)) hr=result->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,&response);
    if(FAILED(hr)) return fail("Finish upload",hr);
    if(response!=0x2001) { std::fprintf(stderr,"Vita response: 0x%04lx\n",response); return 1; }
    return 0;
}

int vendor_cli(Device &device,int argc,wchar_t **argv) {
    if(!wcscmp(argv[1],L"queue") && argc==3) {
        WIN32_FILE_ATTRIBUTE_DATA info{}; if(!GetFileAttributesExW(argv[2],GetFileExInfoStandard,&info)) return 1;
        return vendor_write(device,0x9ff0,{info.nFileSizeLow,info.nFileSizeHigh},argv[2]);
    }
    if(!wcscmp(argv[1],L"upload") && argc==5) {
        wchar_t *end=nullptr; ULONG index=wcstoul(argv[2],&end,10); if(!end || *end) return 2;
        ULONG sidecar=wcstoul(argv[3],&end,10); if(!end || *end || sidecar>1) return 2;
        WIN32_FILE_ATTRIBUTE_DATA info{}; if(!GetFileAttributesExW(argv[4],GetFileExInfoStandard,&info)) return 1;
        return vendor_write(device,0x9ff1,{index,sidecar,info.nFileSizeLow,info.nFileSizeHigh},argv[4]);
    }
    if(!wcscmp(argv[1],L"sync") && argc==2) return vendor_no_data(device,0x9ff2,{});
    if(!wcscmp(argv[1],L"cancel") && argc==2) return vendor_no_data(device,0x9ff5,{});
    std::vector<ULONG> values;
    if(!wcscmp(argv[1],L"status") && argc==2) {
        int code=vendor_no_data(device,0x9ff3,{},&values); if(code) return code;
    } else if(!wcscmp(argv[1],L"item") && argc==3) {
        wchar_t *end=nullptr; ULONG index=wcstoul(argv[2],&end,10); if(!end||*end) return 2;
        int code=vendor_no_data(device,0x9ff4,{index},&values); if(code) return code;
    } else return 2;
    std::fputs("{\"values\":[",stdout); for(size_t i=0;i<values.size();++i)
        std::printf("%s%lu",i?",":"",values[i]); std::fputs("]}\n",stdout); return 0;
}

int space(Device &device) {
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = device.content->Properties(&properties);
    if (FAILED(hr)) return fail("Properties", hr);
    ComPtr<IPortableDeviceValues> values;
    hr = properties->GetValues(L"s10001", nullptr, &values);
    if (FAILED(hr)) return fail("Storage values", hr);
    ULONGLONG capacity = 0, free_bytes = 0;
    hr = values->GetUnsignedLargeIntegerValue(WPD_STORAGE_CAPACITY, &capacity);
    if (FAILED(hr)) return fail("Storage capacity", hr);
    hr = values->GetUnsignedLargeIntegerValue(WPD_STORAGE_FREE_SPACE_IN_BYTES, &free_bytes);
    if (FAILED(hr)) return fail("Storage free bytes", hr);
    if (!capacity || free_bytes > capacity) return fail("Invalid storage space", E_UNEXPECTED);
    std::printf("{\"capacity\":%llu,\"free\":%llu,\"used\":%llu}\n",
                capacity, free_bytes, capacity - free_bytes);
    return 0;
}

int copy(Device &device, const wchar_t *object_id, const wchar_t *destination, bool thumbnail) {
    ComPtr<IPortableDeviceResources> resources;
    HRESULT hr = device.content->Transfer(&resources);
    if (FAILED(hr)) return fail("Transfer", hr);
    DWORD optimal = 0;
    ComPtr<IStream> stream;
    hr = resources->GetStream(object_id, thumbnail ? WPD_RESOURCE_THUMBNAIL : WPD_RESOURCE_DEFAULT, STGM_READ,
                              &optimal, &stream);
    if (FAILED(hr)) return fail("GetStream", hr);
    HANDLE file = CreateFileW(destination, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail("CreateFile", HRESULT_FROM_WIN32(GetLastError()));
    std::vector<uint8_t> buffer(optimal >= 4096 && optimal <= 1024 * 1024 ? optimal : 65536);
    ULONGLONG total = 0;
    while (SUCCEEDED(hr)) {
        ULONG received = 0;
        hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &received);
        if (FAILED(hr) || !received) break;
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), received, &written, nullptr) || written != received) {
            DWORD error = GetLastError();
            hr = HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
            break;
        }
        total += written;
    }
    if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    if (FAILED(hr)) {
        DeleteFileW(destination); // Only this invocation's CREATE_NEW partial file.
        return fail("Read", hr);
    }
    std::printf("{\"bytes\":%llu}\n", total);
    return 0;
}

int thumbs(Device &device, int count, wchar_t **ids) {
    ComPtr<IPortableDeviceResources> resources;
    HRESULT hr = device.content->Transfer(&resources);
    if (FAILED(hr)) return fail("Transfer", hr);
    _setmode(_fileno(stdout), _O_BINARY);
    uint32_t n = static_cast<uint32_t>(count);
    if (fwrite(&n, sizeof(n), 1, stdout) != 1) return 1;
    for (int i = 0; i < count; ++i) {
        DWORD optimal = 0;
        ComPtr<IStream> stream;
        std::vector<uint8_t> image;
        hr = resources->GetStream(ids[i], WPD_RESOURCE_THUMBNAIL, STGM_READ,
                                  &optimal, &stream);
        if (SUCCEEDED(hr)) {
            uint8_t chunk[16384];
            while (image.size() <= 256u * 256u * 3u + 54u) {
                ULONG received = 0;
                hr = stream->Read(chunk, sizeof(chunk), &received);
                if (FAILED(hr) || !received) break;
                image.insert(image.end(), chunk, chunk + received);
            }
            if (FAILED(hr) || image.size() > 256u * 256u * 3u + 54u ||
                image.size() < 54 || image[0] != 'B' || image[1] != 'M') image.clear();
        }
        uint32_t size = static_cast<uint32_t>(image.size());
        if (fwrite(&size, sizeof(size), 1, stdout) != 1 ||
            (size && fwrite(image.data(), 1, size, stdout) != size)) return 1;
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    bool bulk_thumbs = argc >= 3 && argc <= 34 && !wcscmp(argv[1], L"thumbs");
    bool vendor = argc >= 2 && (!wcscmp(argv[1],L"queue") || !wcscmp(argv[1],L"upload") ||
        !wcscmp(argv[1],L"sync") || !wcscmp(argv[1],L"status") ||
        !wcscmp(argv[1],L"item") || !wcscmp(argv[1],L"cancel"));
    if (argc != 2 && argc != 3 && argc != 4 && !bulk_thumbs && !(vendor && argc==5)) {
        std::fputs("usage: vcm-wpd list | space | queue FILE | upload INDEX SIDECAR FILE | sync | status | item INDEX | cancel | copy ID PATH | thumb ID PATH | thumbs ID...\n", stderr);
        return 2;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return fail("CoInitializeEx", hr);
    Device device;
    hr = open_vcm(device, vendor || (argc == 2 && !wcscmp(argv[1], L"probe")));
    int result = FAILED(hr) ? fail("Open VCM device", hr) :
                 vendor ? vendor_cli(device,argc,argv) :
                 argc == 2 && !wcscmp(argv[1], L"list") ? list(device) :
                 argc == 2 && !wcscmp(argv[1], L"space") ? space(device) :
                 argc == 2 && !wcscmp(argv[1], L"probe") ? vendor_probe(device) :
                 argc == 3 && !wcscmp(argv[1], L"attributes") ? attributes(device, argv[2]) :
                 bulk_thumbs ? thumbs(device, argc - 2, argv + 2) :
                 argc == 4 && !wcscmp(argv[1], L"copy") ? copy(device, argv[2], argv[3], false) :
                 argc == 4 && !wcscmp(argv[1], L"thumb") ? copy(device, argv[2], argv[3], true) : 2;
    device.content.Reset();
    device.device.Reset();
    CoUninitialize();
    return result;
}
