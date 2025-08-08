#include <iostream>
#include <cstdlib>
#include <sys/inotify.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cstring>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

using namespace std;

// K8S-konfiguraatio
const string container_name = "pm-collector";
const string container_namespace = "oam"; // Muutettava tarpeen tullessa
const string watch_dir = "/mnt/kpi/";  // Polku kontin sisällä
const string target_file = "oam_kpi.tgz"; // Havaitun tiedosto nimi
const string local_download_path = "/tmp/";  // Haetun tiedoston tallennus paikka paikallisella työasemalla

// KPI Reporter API-konfiguraatio
const string KPI_REPORTER_API_URL = "http://kpireporter.nokia.com/api/upload";
const string API_KEY = "your-api-key"; // Korvaa varsinaisella API-avaimella

// Funktio .tgz tiedosto importtaamiseen KPI Reporteriin
bool importKpiPackage(const string& filePath) {
    CURL* curl;
    CURLcode res;
    struct curl_httppost* formpost = nullptr;
    struct curl_httppost* lastptr = nullptr;
    struct curl_slist* headers = nullptr;

    // Alusta libcurl
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl) {
        cerr << "Failed to initialize CURL" << endl;
        return false;
    }

    // Lisää .tgz tiedosto POST-pyyntöön
    curl_formadd(&formpost, &lastptr,
                 CURLFORM_COPYNAME, "file",
                 CURLFORM_FILE, filePath.c_str(),
                 CURLFORM_END);

    // Lisää API avain tarvittaessa (valinnainen)
    string auth_header = "Authorization: Bearer " + API_KEY;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: multipart/form-data");

    // Aseta CURL-optiot
    curl_easy_setopt(curl, CURLOPT_URL, KPI_REPORTER_API_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Suorita pyyntö
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        cerr << "CURL error: " << curl_easy_strerror(res) << endl;
        return false;
    }

    // Siivous
    curl_easy_cleanup(curl);
    curl_formfree(formpost);
    curl_slist_free_all(headers);
    curl_global_cleanup();

    cout << "KPI package successfully uploaded to KPI Reporter!" << endl;
    return true;
}

// Funktio KPI-paketin hakemiseksi Kubernetes-säiliöstä
void retrieveFile() {
    string full_container_path = watch_dir + "/" + target_file;
    string local_file_path = local_download_path + target_file;

    string copy_cmd = "kubectl cp " + container_namespace + "/" + container_name + ":" + full_container_path + " " + local_file_path;
    cout << "File detected! Retrieving it using: " << copy_cmd << endl;
    
    int status = system(copy_cmd.c_str());
    if (status == 0) {
        cout << "File successfully retrieved: " << local_file_path << endl;

        // API komento tiedoston lataamiseksi
        if (importKpiPackage(local_file_path)) {
            cout << "File successfully imported into KPI Reporter." << endl;
        } else {
            cerr << "Failed to upload the file to KPI Reporter!" << endl;
        }
    } else {
        cerr << "Failed to retrieve the file!" << endl;
    }
}

// Funktio kontin valvonnalle uusien KPI tiedostojen havaitsemiseen
void monitorFile() {
    int fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        exit(EXIT_FAILURE);
    }

    int wd = inotify_add_watch(fd, watch_dir.c_str(), IN_CREATE);
    if (wd == -1) {
        cerr << "Cannot watch directory: " << watch_dir << endl;
        close(fd);
        exit(EXIT_FAILURE);
    }

    char buffer[BUF_LEN];

    while (true) {
        int length = read(fd, buffer, BUF_LEN);
        if (length < 0) {
            perror("read");
            exit(EXIT_FAILURE);
        }

        int i = 0;
        while (i < length) {
            struct inotify_event* event = (struct inotify_event*)&buffer[i];
            if (event->mask & IN_CREATE) {
                string detected_file(event->name);
                cout << "New file detected: " << detected_file << endl;
                if (detected_file == target_file) {
                    retrieveFile();
                    break; // Poistutaan haun jälkeen
                }
            }
            i += EVENT_SIZE + event->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
}

int main() {
    cout << "Monitoring Kubernetes container for KPI package..." << endl;
    monitorFile();
    return 0;
}